/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <stdarg.h>
#include "qbt.h"
#include "qbt_geni.h"
#include "qbt_transport.h"
#include "UkeBluetoothClock.h"
#include "SmmuBypass.h"
#include "UkeUsbBoard.h"
#include "WcnPower.h"
#include "BluetoothFirmware.h"

extern "C" int armcpu_spi_attach(uint32_t,bool,void(*)(void *),void *,uint32_t *,uint64_t *);
extern "C" int armcpu_spi_share(uint32_t,void(*)(void *),void *);
extern "C" int armcpu_spi_mask(uint32_t,bool);
extern "C" volatile int native_gpu_addon_stage;
extern "C" volatile int32_t native_wifi_wpss_stats[16];
extern "C" {
volatile uint64_t native_bluetooth_stats[32];
uint8_t native_bluetooth_boot_rx[512];
volatile uint32_t native_bluetooth_boot_rx_length;
uint8_t native_bluetooth_boot_tail[256];
volatile uint32_t native_bluetooth_boot_tail_length;
/* log-bluetooth=2: pins before TX, RX bytes before TX, then pins, engine status and download reports when startup returns. */
volatile uint32_t native_bluetooth_boot_state[12];
/* log-bluetooth=2: live pid and starts seen for bluetoothd, then for BlueTool. */
volatile uint32_t native_bluetooth_processes[4];
/* log-bluetooth=2: printable runs in controller output, which carry its assert text. */
char *volatile native_bluetooth_boot_text;
volatile uint32_t native_bluetooth_boot_text_length;
/* In-band sleep: wake indications sent, wake acknowledgements sent, sleep indications sent, clock stops undone by a wake
 * inside the stop, writes an idle UART refused, interrupts of the shared line that found the wake disarmed, RX stop timeouts, wakes taken. */
volatile uint64_t native_bluetooth_ibs[8];
}
namespace {
struct Region {IOMemoryMap *map;volatile uint32_t *va;};
constexpr unsigned kLoopbackBytes=128;
struct Bluetooth {
    Region gcc,uart,wrapper,tlmm,smmu;
    IOService *radio;
    IOBufferMemoryDescriptor *dma;
    struct smmu_plan domain;
    uint64_t dma_address;
    struct qbt_geni geni;
    struct qbt *core;
    uint64_t rx_sequence;
    uint32_t pending,rx_wake,power_request,started;
    unsigned log,baud;
    uint8_t address[6];
    bool resources,powered,gated,clocked,serial_irq,wake_irq,service,initializing,loopback;
    uint8_t loop_rx[kLoopbackBytes];unsigned loop_length;
} bt;
constexpr unsigned kRx=43,kBtEnable=156,kSwCtrl=80;
constexpr uint32_t kBtEnableCfg=1u|(1u<<9); // OEM bt_en: 2 mA, pull down, output
constexpr uint32_t kTlmmPadConfig=0x3ff;
constexpr uint32_t kEgpioPresent=1u<<11,kEgpioAppsOwner=1u<<12;

constexpr uint32_t btEnableControl(uint32_t current) {
    const uint32_t owner=(current&kEgpioPresent)?kEgpioAppsOwner:0;
    return (current&~kTlmmPadConfig)|kBtEnableCfg|owner;
}
// Cliffs pinctrl routes GPIO summary interrupts to KPSS via INTR_CFG[10:8].
constexpr uint32_t kRxWakeEnable=1u;
constexpr uint32_t kRxWakeFields=(7u<<8)|(1u<<4)|(3u<<2)|(1u<<1)|kRxWakeEnable;
constexpr uint32_t kRxWakeRoute=(3u<<8)|(1u<<4)|(2u<<2)|(1u<<1);

constexpr uint32_t rxWakeControl(uint32_t current) {
    return (current&~kRxWakeFields)|kRxWakeRoute;
}
constexpr uint32_t kDmaBytes=32768;
constexpr uint32_t kGeniIos=0x908; // SE_GENI_IOS, Cliffs OEM geni-se.h
constexpr uint32_t kGeniCtsHigh=1u<<1; // IO2_DATA_IN, active-low CTS
bool map(Region &r,uint64_t base,size_t bytes) {
    if(r.va)return true;
    IODeviceMemory *memory=IODeviceMemory::withRange(base,bytes);
    r.map=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    if(memory)memory->release();r.va=r.map?reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()):nullptr;
    return r.va!=nullptr;
}
void barrier(){__asm__ volatile("dsb sy" ::: "memory");}
uint64_t nowNs(void *){uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns;}
void wake(void *){__atomic_store_n(&bt.pending,1u,__ATOMIC_RELEASE);thread_wakeup((event_t)&bt.pending);}
void waitUntil(void *,uint64_t deadline) {
    if(__atomic_exchange_n(&bt.pending,0u,__ATOMIC_ACQ_REL))return;
    if(deadline==UINT64_MAX)assert_wait((event_t)&bt.pending,THREAD_UNINT);
    else {uint64_t absolute;nanoseconds_to_absolutetime(deadline,&absolute);assert_wait_deadline((event_t)&bt.pending,THREAD_UNINT,absolute);}
    if(__atomic_exchange_n(&bt.pending,0u,__ATOMIC_ACQ_REL))clear_wait(current_thread(),THREAD_AWAKENED);
    else thread_block(THREAD_CONTINUE_NULL);
}
uint32_t urd(void *,uint32_t reg){return bt.uart.va[reg/4];}
void uwr(void *,uint32_t reg,uint32_t value){bt.uart.va[reg/4]=value;barrier();}
uint32_t grd(void *,uint32_t reg){return bt.gcc.va[reg/4];}
void gwr(void *,uint32_t reg,uint32_t value){bt.gcc.va[reg/4]=value;barrier();}
void delay(void *,unsigned us){IODelay(us);}
void dmaBarrier(void *){barrier();}
void dmaSync(void *,const struct qbt_dma_buffer *buffer,size_t bytes,bool device,bool tx) {
    // Match the board's touch path: cached RAM behind the firmware bypass bank.
    if(bytes && (device || !tx)) {
        uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
        size_t line=4u<<((ctr>>16)&15);
        uintptr_t at=reinterpret_cast<uintptr_t>(buffer->bytes)&~(line-1);
        uintptr_t end=reinterpret_cast<uintptr_t>(buffer->bytes)+bytes;
        for(;at<end;at+=line) {
            if(tx)__asm__ volatile("dc cvac, %0"::"r"(at):"memory");
            else if(device)__asm__ volatile("dc civac, %0"::"r"(at):"memory");
            else __asm__ volatile("dc ivac, %0"::"r"(at):"memory");
        }
    }
    barrier();
}
uint32_t srd(void *,uint32_t reg){return bt.smmu.va[reg/4];}
void swr(void *,uint32_t reg,uint32_t value){bt.smmu.va[reg/4]=value;barrier();}
void irqMask(void *,bool masked) {
    if(bt.serial_irq)(void)armcpu_spi_mask(616u,masked);
}
void serialIrq(void *){qbt_geni_irq(&bt.geni);}
void pin(unsigned gpio,uint32_t cfg,int output=-1) {
    if(output>=0)bt.tlmm.va[(gpio*4096+4)/4]=output?2:0;
    bt.tlmm.va[gpio*4096/4]=cfg;barrier();
}
bool setBtEnable(bool enabled) {
    auto *gpio=bt.tlmm.va+(kBtEnable*4096u)/4u;
    const uint32_t desired=btEnableControl(gpio[0]);
    // Stage the output latch before claiming APSS ownership or enabling output.
    const uint32_t output=enabled?2u:0u;
    gpio[1]=(gpio[1]&~2u)|output;
    barrier();
    gpio[0]=desired;
    barrier();
    const uint32_t fields=kTlmmPadConfig|(desired&kEgpioAppsOwner);
    const uint32_t actual=gpio[0], levels=gpio[1];
    if((actual&fields)==(desired&fields) && (levels&2u)==output)return true;
    printf("[bluetooth] BT_EN write failed enabled=%u ctl=%08x wanted=%08x io=%08x\n",
           enabled,actual,desired,levels);
    return false;
}
void rxIrq(void *) {
    volatile uint32_t *r=bt.tlmm.va+kRx*4096/4;
    if(!(r[3]&1))return;
    /* Raw status stays on after the wake is disarmed, so every UART start bit latches it again, and each pin of the
     * shared TLMM summary line calls this handler. Only the armed interrupt is a wake. */
    if(!(r[2]&kRxWakeEnable)){native_bluetooth_ibs[5]=native_bluetooth_ibs[5]+1;return;}
    r[2]&=~kRxWakeEnable;r[3]=0;barrier();
    native_bluetooth_ibs[7]=native_bluetooth_ibs[7]+1;
    __atomic_store_n(&bt.rx_wake,1u,__ATOMIC_RELEASE);wake(nullptr);
}
void disarmRxWake(){bt.tlmm.va[(kRx*4096+8)/4]&=~kRxWakeEnable;bt.tlmm.va[(kRx*4096+12)/4]=0;pin(kRx,1u<<2);}
bool serialClock(bool enabled) {
    if(!uke_bluetooth_clock_vote(bt.gcc.va+0x52010/4,enabled))return false;
    if(!enabled){bt.clocked=false;return true;}
    for(unsigned i=0;i<1000;i++){if(!(grd(nullptr,0x1e274)&(1u<<31))){bt.clocked=true;return true;}IODelay(1);}
    return false;
}
size_t rxBytes(void *,const uint8_t *bytes,size_t n) {
    if(bt.loopback) {
        size_t count=sizeof(bt.loop_rx)-bt.loop_length;if(count>n)count=n;
        memcpy(bt.loop_rx+bt.loop_length,bytes,count);bt.loop_length+=(unsigned)count;
        return n;
    }
    if(bt.log>=2 && bt.initializing) {
        uint32_t at=__atomic_load_n(&native_bluetooth_boot_rx_length,__ATOMIC_RELAXED);
        size_t count=sizeof(native_bluetooth_boot_rx)-at;if(count>n)count=n;
        if(count) {
            memcpy(native_bluetooth_boot_rx+at,bytes,count);
            __atomic_store_n(&native_bluetooth_boot_rx_length,at+(uint32_t)count,__ATOMIC_RELEASE);
        }
        static char run[96];static unsigned runLength;
        for(size_t i=0;i<n;i++) {
            uint8_t c=bytes[i];
            if(c>=32 && c<127 && runLength<sizeof(run)-1){run[runLength++]=(char)c;continue;}
            if(runLength>=12 && memchr(run,' ',runLength)) {
                uint32_t at=native_bluetooth_boot_text_length;
                if(native_bluetooth_boot_text && at+runLength+1<8192) {
                    memcpy(native_bluetooth_boot_text+at,run,runLength);native_bluetooth_boot_text[at+runLength]='|';
                    __atomic_store_n(&native_bluetooth_boot_text_length,at+runLength+1,__ATOMIC_RELEASE);
                }
            }
            runLength=0;
        }
        uint32_t tail=__atomic_load_n(&native_bluetooth_boot_tail_length,__ATOMIC_RELAXED);
        for(size_t i=0;i<n;i++)native_bluetooth_boot_tail[(tail+i)%sizeof(native_bluetooth_boot_tail)]=bytes[i];
        __atomic_store_n(&native_bluetooth_boot_tail_length,tail+(uint32_t)n,__ATOMIC_RELEASE);
    }
    bt.rx_sequence+=n;return qbt_receive(bt.core,bytes,n);
}
size_t nativeRx(void *,const uint8_t *bytes,size_t n){return qbt_transport_receive(bytes,(uint32_t)n);}
size_t writeUart(void *,const uint8_t *p,size_t n){return qbt_geni_write(&bt.geni,p,n);}
bool drained(void *){return bt.gated || qbt_geni_drained(&bt.geni);}
int flow(void *,bool enabled){qbt_geni_set_flow(&bt.geni,enabled);return 0;}
void pump(void *) {
    if(bt.log>=2 && bt.initializing) {
        struct qbt_stats stats;qbt_get_stats(bt.core,&stats);
        native_bluetooth_stats[3]=stats.tx_bytes;native_bluetooth_stats[4]=stats.rx_bytes;
        native_bluetooth_boot_state[10]=(uint32_t)stats.tlv_acks|((uint32_t)stats.tlv_sent<<16);
        native_bluetooth_boot_state[11]=(uint32_t)stats.hardware_errors|((uint32_t)stats.last_hardware_error<<8)|((uint32_t)stats.tlv_final_status<<16);
    }
    if(__atomic_exchange_n(&bt.rx_wake,0u,__ATOMIC_ACQ_REL) && bt.powered) {
        qbt_rx_wake(bt.core);
        /* The GPIO consumed the first IBS wake byte while the UART was gated. It counts as received, so a clock stop
         * in progress (quiesce() pumps too) sees it and backs out. */
        const uint8_t wakeByte=0xfd;bt.rx_sequence++;(void)qbt_receive(bt.core,&wakeByte,1);
    }
    if(!bt.gated) {
        if(qbt_geni_service(&bt.geni))wake(nullptr);
        if(bt.geni.state==QBT_GENI_FAILED)qbt_fail(bt.core,bt.geni.error);
    }
}
int quiesce() {
    uint64_t end=nowNs(nullptr)+40000000ULL;
    for(;;) {
        int e=qbt_geni_quiesce(&bt.geni);if(!e)return 0;
        if(e!=-EINPROGRESS && e!=-EBUSY)return e;
        pump(nullptr);if(bt.geni.state==QBT_GENI_STOPPED)return 0;
        if(nowNs(nullptr)>=end)return -ETIMEDOUT;
        uint64_t next=qbt_geni_deadline(&bt.geni);if(next>end)next=end;waitUntil(nullptr,next);
    }
}
int clockVote(void *,bool enabled) {
    if(enabled) {
        if(!bt.gated)return 0;
        if(!serialClock(true))return -EIO;
        disarmRxWake();IODelay(10);bt.gated=false;
        return qbt_geni_resume(&bt.geni);
    }
    if(bt.gated)return 0;
    uint64_t sequence=bt.rx_sequence;
    int e=quiesce();if(e)return e;
    if(sequence!=bt.rx_sequence || qbt_transport_tx_pending()) {qbt_geni_resume(&bt.geni);return -EBUSY;}
    volatile uint32_t *r=bt.tlmm.va+kRx*4096/4;
    // Program the target and trigger with parent delivery masked.
    const uint32_t config=rxWakeControl(r[2]);
    r[2]=config;
    barrier();
    if((r[2]&kRxWakeFields)!=kRxWakeRoute) {
        disarmRxWake();qbt_geni_resume(&bt.geni);return -EIO;
    }
    pin(kRx,3u);
    r[3]=0;
    barrier();
    r[2]=config|kRxWakeEnable;
    barrier();
    if(!(r[1]&1) || (r[3]&1)) {disarmRxWake();qbt_geni_resume(&bt.geni);return -EBUSY;}
    /* Leave RTS ready so the controller can send its IBS wake indication. */
    uwr(nullptr,0x2ac,0x80000001u);
    bt.gated=true;
    if(!serialClock(false)){bt.gated=false;disarmRxWake();qbt_geni_resume(&bt.geni);return -EIO;}
    return 0;
}
int setRate(void *,uint32_t baud) {
    int e=quiesce();if(e)return e;
    uint32_t hz=0;struct uke_bluetooth_clock_io io={nullptr,grd,gwr,delay};
    e=uke_bluetooth_clock_source(&io,baud,&hz);
    if(!e)e=qbt_geni_set_rate(&bt.geni,baud,hz,16);
    if(!e)e=qbt_geni_resume(&bt.geni);
    if(!e)bt.baud=baud;return e;
}
void *allocate(void *,size_t n){return IOMallocData(n);}
void release(void *,void *p,size_t n){IOFreeData(p,n);}
int firmware(void *,const char *name,const uint8_t **data,size_t *size) {
    uint8_t *bytes=nullptr;int e=uke_bt_firmware_read(name,&bytes,size);*data=bytes;return -e;
}
void firmwarePut(void *,const uint8_t *data,size_t size){uke_bt_firmware_release(const_cast<uint8_t *>(data),size);}
void log(void *,int level,const char *line){if(level<=3 || bt.log)printf("[bluetooth] %s\n",line);}
bool address() {
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    OSData *id=chosen?OSDynamicCast(OSData,chosen->getProperty("unique-chip-id")):nullptr;
    if(!id || id->getLength()<8){if(chosen)chosen->release();return false;}
    uint64_t hash=0xcbf29ce484222325ULL;
    static const char domain[]="unixnu-bluetooth";
    for(unsigned i=0;i<sizeof(domain)-1;i++)hash=(hash^(uint8_t)domain[i])*0x100000001b3ULL;
    const uint8_t *seed=static_cast<const uint8_t *>(id->getBytesNoCopy());
    bool nonzero=false;
    for(unsigned i=0;i<id->getLength();i++){nonzero|=seed[i]!=0;hash=(hash^seed[i])*0x100000001b3ULL;}
    bool published=false;
    if(nonzero) {
        for(unsigned i=0;i<6;i++)bt.address[i]=(uint8_t)(hash>>(i*8));
        bt.address[0]=(bt.address[0]&0xfe)|2;
        IORegistryEntry *node=IORegistryEntry::fromPath("/arm-io/bluetooth",gIODTPlane);
        OSData *encoding=node?OSDynamicCast(OSData,node->getProperty("transport-encoding")):nullptr;
        OSData *speed=node?OSDynamicCast(OSData,node->getProperty("transport-speed")):nullptr;
        uint32_t wire=0,baud=0;
        if(encoding && encoding->getLength()==4)memcpy(&wire,encoding->getBytesNoCopy(),4);
        if(speed && speed->getLength()==4)memcpy(&baud,speed->getBytesNoCopy(),4);
        if(node && wire==1 && baud==3000000) {
            published=chosen->setProperty("mac-address-bluetooth0",bt.address,6) &&
                node->setProperty("local-mac-address",bt.address,6);
            if(published && !bt.radio) {
                IOService *radio=new IOService;
                if(radio && radio->init()) {
                    radio->setName("bluetooth");
                    published=radio->setProperty("device_type","bluetooth") &&
                        radio->setProperty("transport-encoding",encoding) &&
                        radio->setProperty("transport-speed",speed) &&
                        radio->setProperty("local-mac-address",bt.address,6);
                    IOService *provider=IOService::getPlatform();
                    if(published && provider && radio->attach(provider)) {
                        bt.radio=radio;radio->registerService();
                    } else {published=false;radio->release();}
                } else {published=false;if(radio)radio->release();}
            }
        }
        if(node)node->release();
    }
    chosen->release();return published;

}
int resources() {
    if(bt.resources)return 0;
    if(!map(bt.gcc,0x100000,0x80000) || !map(bt.uart,0x888000,0x4000) ||
        !map(bt.wrapper,0x8c0000,0x2000) || !map(bt.tlmm,0xf100000,0x9d000) || !map(bt.smmu,0x15000000,0xd0000))return -ENOMEM;
    if(!address())return -ENOENT;
    const IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(!bt.dma)bt.dma=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,options,kDmaBytes,4096);
    if(!bt.dma)return -ENOMEM;
    IOByteCount contiguous=0;
    uint64_t pa=bt.dma->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);
    if(!pa || contiguous<kDmaBytes)return -EIO;
    AppsSmmuLock guard;if(!guard.ready())return -ENOMEM;
    struct smmu_io io={nullptr,srd,swr};
    bt.domain=smmu_plan_bypass(&io,0x423,pa,kDmaBytes);
    const char *error=bt.domain.error?bt.domain.error:smmu_configure_bypass(&io,&bt.domain,0x423);
    if(error) {
        printf("[bluetooth dma] slot=%u bank=%u result=%s\n",bt.domain.slot,bt.domain.bank,error);
        return -EIO;
    }
    bt.dma_address=pa;
    printf("[bluetooth dma] bypass slot=%u bank=%u pa=%llx bytes=%u\n",bt.domain.slot,bt.domain.bank,pa,kDmaBytes);
    bt.resources=true;return 0;
}
int stopEngine() {
    irqMask(nullptr,true);uwr(nullptr,0xe1c,0);
    if((urd(nullptr,0x40)&0x1001) || (urd(nullptr,0xe40)&3)) {
        uwr(nullptr,0x604,2);uwr(nullptr,0x634,2);
        unsigned i=0;for(;i<1000 && (urd(nullptr,0x40)&0x1001);i++)IODelay(1);
        if(i==1000)return -ETIMEDOUT;
        uwr(nullptr,0xc58,1);uwr(nullptr,0xd58,1);
        for(i=0;i<1000 && (urd(nullptr,0xe40)&3);i++)IODelay(1);
        if(i==1000)return -ETIMEDOUT;
    }
    uwr(nullptr,0x618,UINT32_MAX);uwr(nullptr,0x648,UINT32_MAX);
    uwr(nullptr,0xc44,UINT32_MAX);uwr(nullptr,0xd44,UINT32_MAX);
    bt.geni.state=QBT_GENI_OFF;bt.geni.rx_busy=bt.geni.tx_busy=false;
    return 0;
}
uint32_t linePins() {
    static const uint8_t pins[]={40,41,42,43,kBtEnable,kSwCtrl};
    uint32_t bits=0;
    for(unsigned i=0;i<sizeof(pins);i++)bits|=(bt.tlmm.va[(pins[i]*4096u+4)/4]&1u)<<i;
    return bits;
}
int prepareEngine() {
    uint8_t *bytes=static_cast<uint8_t *>(bt.dma->getBytesNoCopy());
    struct qbt_dma_buffer tx={bytes,bt.dma_address,QBT_GENI_TX_SIZE},rx[QBT_GENI_RX_COUNT];
    for(unsigned i=0;i<QBT_GENI_RX_COUNT;i++)rx[i]={bytes+4096+i*QBT_GENI_RX_SIZE,bt.dma_address+4096+i*QBT_GENI_RX_SIZE,QBT_GENI_RX_SIZE};
    struct qbt_geni_ops ops={nullptr,urd,uwr,dmaBarrier,dmaSync,nowNs,irqMask,wake,rxBytes};
    int e=qbt_geni_prepare(&bt.geni,&ops,&tx,rx);if(e)return e;
    native_bluetooth_stats[14]=5;
    uint32_t hz;struct uke_bluetooth_clock_io clocks={nullptr,grd,gwr,delay};
    e=uke_bluetooth_clock_source(&clocks,115200,&hz);if(e)return e;
    e=qbt_geni_set_rate(&bt.geni,115200,hz,16);if(e)return e;
    bt.baud=115200;return 0;
}
/* log-bluetooth=2, controller still in reset: GENI's internal loopback checks DMA,
 * cache maintenance and packing without the controller. Stats[30] holds received
 * length, bit 16 sent, bit 17 match; stats[31] the first received bytes. */
int loopbackCheck() {
    static const uint8_t sequence[16]={0x01,0x55,0xaa,0x00,0xff,0x04,0x0e,0x7e,0x81,0x42,0x24,0x18,0x10,0x20,0x40,0x80};
    uint8_t pattern[kLoopbackBytes];
    for(unsigned i=0;i<sizeof(pattern);i++)pattern[i]=sequence[i%sizeof(sequence)];
    uwr(nullptr,0x22c,3);bt.loopback=true;bt.loop_length=0;
    int e=qbt_geni_start(&bt.geni);
    const uint64_t start=nowNs(nullptr),end=start+50000000ULL;
    const size_t sent=e?0:qbt_geni_write(&bt.geni,pattern,sizeof(pattern));
    uint64_t drained=0;
    while(sent && bt.geni.state==QBT_GENI_RUN && nowNs(nullptr)<end) {
        qbt_geni_service(&bt.geni);
        if(!drained && qbt_geni_drained(&bt.geni))drained=nowNs(nullptr);
        if(drained && bt.loop_length==sizeof(pattern))break;
        IODelay(10);
    }
    const bool match=bt.loop_length==sizeof(pattern) && !memcmp(bt.loop_rx,pattern,sizeof(pattern));
    printf("[bluetooth clock] loop sent=%u received=%u match=%u drained_us=%llu expected_us=%llu rcg=%08x/%08x/%08x/%08x dfs=%08x\n",
           (unsigned)sent,bt.loop_length,match,
           (unsigned long long)(drained?(drained-start)/1000:0),
           (unsigned long long)(10000000ULL*sizeof(pattern)/115200),
           grd(nullptr,0x1e284),grd(nullptr,0x1e288),grd(nullptr,0x1e28c),
           grd(nullptr,0x1e290),grd(nullptr,0x1e294));
    uint64_t first=0;memcpy(&first,bt.loop_rx,bt.loop_length<8?bt.loop_length:8);
    native_bluetooth_stats[31]=first;
    native_bluetooth_stats[30]=bt.loop_length|(sent?1u<<16:0)|(match?1u<<17:0);
    /* Stopping exercises the same cancel path as a baud change. */
    e=bt.geni.state==QBT_GENI_RUN?quiesce():-EIO;
    bt.loopback=false;uwr(nullptr,0x22c,0);
    if(!e && bt.geni.state==QBT_GENI_STOPPED)return 0;
    e=stopEngine();if(e)return e;
    return prepareEngine();
}
int powerOn() {
    if(bt.powered)return 0;
    native_bluetooth_stats[14]=1;int e=resources();if(e)return e;
    native_bluetooth_stats[14]=2;
    e=uke_wcn_power_get(UKE_WCN_BLUETOOTH);if(e)return -e;
    bt.powered=true;bt.gated=true;
    native_bluetooth_stats[14]=3;
    pin(40,0);pin(41,1);pin(42,3);pin(43,1);
    pin(kSwCtrl,0);
    if(!setBtEnable(false))return -EIO;
    // Match the stock power driver's cold sequence before opening ttyHS0.
    IOSleep(150);
    if(!serialClock(true))return -EIO;
    bt.gated=false;
    native_bluetooth_stats[11]=urd(nullptr,0x68);native_bluetooth_stats[12]=bt.wrapper.va[1];
    native_bluetooth_stats[13]=grd(nullptr,0x52010);native_bluetooth_stats[14]=4;
    if(bt.wrapper.va[1]<0x20050000)return -ENOTSUP;
    if(((urd(nullptr,0x68)>>8)&255)!=2)return -ENXIO;
    e=stopEngine();if(e)return e;
    e=prepareEngine();if(e)return e;
    if(!bt.serial_irq){e=armcpu_spi_attach(616u,false,serialIrq,nullptr,nullptr,nullptr);if(e)return -EIO;bt.serial_irq=true;irqMask(nullptr,true);}
    if(!bt.wake_irq){e=armcpu_spi_share(240,rxIrq,nullptr);if(e)return -EIO;bt.wake_irq=true;}
    if(bt.log>=2){e=loopbackCheck();if(e)return e;}
    // Recovery's successful OEM trace powers the controller before ttyHS0
    // opens. Keep the pads parked until BT_EN has settled, then start RX once.
    if(!setBtEnable(true))return -EIO;
    IOSleep(50);
    printf("[bluetooth] SW_CTRL=%u\n", (bt.tlmm.va[(kSwCtrl*4096+4)/4]&1u));
    pin(40,1u<<2);pin(41,(1u<<2)|1u);pin(42,(1u<<2)|3u);pin(43,1u<<2);
    disarmRxWake();
    bt.geni.skip_rx_break=true;
    e=qbt_geni_start(&bt.geni);if(e)return e;
    bt.initializing=true;
    qbt_geni_set_flow(&bt.geni,true);
    native_bluetooth_stats[14]=6;
    const uint32_t ios=urd(nullptr,kGeniIos);
    // Kick one ROM query if the UART sees deasserted CTS. Keep RX ready.
    // qbt_boot_controller restores normal CTS when it changes to 3 Mbaud.
    const bool kickRom=(ios&kGeniCtsHigh)!=0;
    if(kickRom)qbt_geni_set_tx_cts(&bt.geni,false);
    if(bt.log>=2) {
        native_bluetooth_boot_state[0]=linePins();
        native_bluetooth_boot_state[1]=__atomic_load_n(&native_bluetooth_boot_rx_length,__ATOMIC_ACQUIRE);
        printf("[bluetooth rom] pre pins=%02x ios=%08x rfr=%08x clk=%08x/%08x src=%u sample=%u loop=%05x rx=%u cts_override=%u bt_en=%08x/%08x\n",
               native_bluetooth_boot_state[0], ios, urd(nullptr,0x2ac),
               urd(nullptr,0x48), urd(nullptr,0x4c), bt.geni.source_hz, bt.geni.oversampling,
               (unsigned)native_bluetooth_stats[30], native_bluetooth_boot_state[1], kickRom,
               bt.tlmm.va[kBtEnable*4096/4], bt.tlmm.va[(kBtEnable*4096+4)/4]);
    }
    const qbt_geni_stats before=bt.geni.stats;
    e=qbt_boot_controller(bt.core,QBT_CONTROLLER_WCN6750,3000000,bt.address);
    bt.initializing=false;
    if(kickRom && e && bt.geni.state==QBT_GENI_RUN)qbt_geni_set_tx_cts(&bt.geni,true);
    if(bt.log>=2) {
        static const uint32_t regs[]={0x40,0x610,0x640,0xc40,0xd40,0x800,0xd54};
        native_bluetooth_boot_state[2]=linePins();
        for(unsigned i=0;i<7;i++)native_bluetooth_boot_state[3+i]=bt.clocked?urd(nullptr,regs[i]):0;
        struct qbt_stats stats;qbt_get_stats(bt.core,&stats);
        native_bluetooth_boot_state[10]=(uint32_t)stats.tlv_acks|((uint32_t)stats.tlv_sent<<16);
        native_bluetooth_boot_state[11]=(uint32_t)stats.hardware_errors|((uint32_t)stats.last_hardware_error<<8)|((uint32_t)stats.tlv_final_status<<16);
        const auto &after=bt.geni.stats;
        printf("[bluetooth rom] result=%d pins=%02x ios=%08x status=%08x accepted=%llu tx=%llu rx=%llu txdma=%llu rxdma=%llu irq=%llu line=%llu malformed=%llu unmatched=%llu\n",
               e, native_bluetooth_boot_state[2], urd(nullptr,kGeniIos), native_bluetooth_boot_state[3],
               (unsigned long long)stats.tx_bytes, (unsigned long long)(after.tx_bytes-before.tx_bytes), (unsigned long long)(after.rx_bytes-before.rx_bytes),
               (unsigned long long)(after.tx_dma-before.tx_dma), (unsigned long long)(after.rx_dma-before.rx_dma),
               (unsigned long long)(after.interrupts-before.interrupts), (unsigned long long)(after.line_errors-before.line_errors),
               (unsigned long long)stats.malformed, (unsigned long long)stats.unmatched_events);
        if(e) {
            // Only pre-service controller startup bytes, bounded to 32 bytes.
            char bytes[65]={};
            const char hex[]="0123456789abcdef";
            uint32_t length=__atomic_load_n(&native_bluetooth_boot_rx_length,__ATOMIC_ACQUIRE);
            uint32_t start=native_bluetooth_boot_state[1];
            uint32_t count=length-start;
            if(count>32)count=32;
            for(uint32_t i=0;i<count;i++) {
                uint8_t byte=native_bluetooth_boot_rx[start+i];
                bytes[i*2]=hex[byte>>4];bytes[i*2+1]=hex[byte&15];
            }
            printf("[bluetooth rom] rx_head=%s txfifo=%08x dma_irq=%08x/%08x fault=%08x/%08x/%08x/%08x\n",
                   bytes, native_bluetooth_boot_state[8], native_bluetooth_boot_state[6], native_bluetooth_boot_state[7],
                   bt.geni.fault_irq[0], bt.geni.fault_irq[1], bt.geni.fault_irq[2], bt.geni.fault_irq[3]);
        }
    }
    return e;
}
int powerOff() {
    if(!bt.powered)return 0;
    qbt_set_service(bt.core,false);bt.service=false;
    int e=0;
    if(bt.geni.state!=QBT_GENI_OFF) {
        if(bt.gated){int resumed=clockVote(nullptr,true);if(!e)e=resumed;}
        if(bt.clocked){int shutdown=qbt_shutdown(bt.core);if(!e)e=shutdown;}
    }
    if(bt.clocked && bt.geni.state!=QBT_GENI_OFF){int stopped=quiesce();if(!e)e=stopped;}
    irqMask(nullptr,true);disarmRxWake();
    if(!setBtEnable(false) && !e)e=-EIO;
    IOSleep(100);
    int retired=bt.clocked?stopEngine():0;if(!e)e=retired;
    if(retired)return e;
    bt.gated=serialClock(false);
    if(!bt.gated)return e ? e : -EIO;
    bt.powered=false;__atomic_store_n(&bt.rx_wake,0u,__ATOMIC_RELEASE);
    int released=uke_wcn_power_put(UKE_WCN_BLUETOOTH);if(!e)e=-released;
    return e;
}
int requestPower(void *,bool enabled) {
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&bt.power_request,&expected,enabled?1u:2u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return EBUSY;
    wake(nullptr);return 0;
}
void powerResult(bool enabled,int error) {
    struct qbt_info info;qbt_get_info(bt.core,&info);
    native_bluetooth_stats[17]=info.product;native_bluetooth_stats[18]=info.soc;
    native_bluetooth_stats[19]=((uint32_t)info.rom<<16)|info.patch;
    if(enabled && !error)native_bluetooth_stats[14]=7;
    qbt_transport_power_result(enabled,error<0?-error:error,info.firmware,info.baud);
    native_bluetooth_stats[20]=(unsigned)bt.powered|((unsigned)bt.gated<<1)|((unsigned)bt.clocked<<2);
    native_bluetooth_stats[21]=uke_wcn_power_owners();
    native_bluetooth_stats[0]=error?3:enabled?2:0;native_bluetooth_stats[1]=(uint64_t)(int64_t)error;
    printf("[bluetooth] power=%u state=%u error=%d baud=%u soc=%x rom=%x firmware=%s\n",
           enabled, error ? 3 : info.state, error, info.baud, info.soc, info.rom, info.firmware);
    if(error && bt.geni.error)printf("[bluetooth uart] error=%d irq=%x/%x/%x/%x\n",bt.geni.error,bt.geni.fault_irq[0],bt.geni.fault_irq[1],bt.geni.fault_irq[2],bt.geni.fault_irq[3]);
}
void worker(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"bluetooth");
    struct qbt_platform ops={nullptr,allocate,release,nowNs,writeUart,drained,setRate,flow,clockVote,pump,waitUntil,nativeRx,firmware,firmwarePut,log,nullptr};
    bt.core=qbt_create(&ops);if(!bt.core){native_bluetooth_stats[0]=3;return;}
    struct qbt_transport_ops transport={nullptr,wake,requestPower};
    int e=qbt_transport_init(&transport);if(e){native_bluetooth_stats[0]=3;return;}
    while(native_gpu_addon_stage<1)waitUntil(nullptr,nowNs(nullptr)+10000000ULL);
    /* Wi-Fi settles WLAN_EN and the shared supplies first; its failure stage is negative. */
    unsigned wifi=3;PE_parse_boot_argn("uke-wifi",&wifi,sizeof(wifi));
    if(wifi && wifi<=3) {
        uint64_t end=nowNs(nullptr)+15000000000ULL;
        while(nowNs(nullptr)<end) {
            int32_t stage=(int32_t)native_wifi_wpss_stats[0];
            if(stage<0 || stage>=13)break;
            waitUntil(nullptr,nowNs(nullptr)+10000000ULL);
        }
    }
    e=powerOn();if(e)(void)powerOff();powerResult(true,e);
    uint64_t health=nowNs(nullptr)+1000000000ULL,closeAt=0;
    for(;;) {
        uint32_t command=__atomic_exchange_n(&bt.power_request,0u,__ATOMIC_ACQ_REL);
        if(command){e=command==1?powerOn():powerOff();if(e && command==1)(void)powerOff();powerResult(command==1,e);}
        if(bt.powered) {
            pump(nullptr);bool active=qbt_transport_active();
            if(active!=bt.service){qbt_set_service(bt.core,active);bt.service=active;closeAt=active?0:nowNs(nullptr)+500000000ULL;}
            uint8_t tx[2048];size_t room=qbt_transmit_capacity(bt.core);
            if(room>sizeof(tx))room=sizeof(tx);
            if(room){uint32_t n=qbt_transport_tx_take(tx,(uint32_t)room);if(n && qbt_transmit(bt.core,tx,n))e=-EIO;}
            qbt_step(bt.core);
            struct qbt_info info;qbt_get_info(bt.core,&info);
            if(info.state==3 || bt.geni.state==QBT_GENI_FAILED || (closeAt && nowNs(nullptr)>=closeAt)) {
                int error=info.state==3?info.error:bt.geni.error;
                (void)powerOff();powerResult(false,error);closeAt=0;
            }
        }
        uint64_t t=nowNs(nullptr);
        if(t>=health) {
            struct qbt_stats stats;qbt_get_stats(bt.core,&stats);
            native_bluetooth_stats[2]=bt.baud;native_bluetooth_stats[3]=stats.tx_bytes;native_bluetooth_stats[4]=stats.rx_bytes;
            native_bluetooth_stats[5]=bt.geni.stats.interrupts;native_bluetooth_stats[6]=stats.timeouts;native_bluetooth_stats[7]=stats.clock_sleeps;
            native_bluetooth_stats[8]=bt.domain.slot;native_bluetooth_stats[9]=bt.domain.bank;
            native_bluetooth_stats[20]=(unsigned)bt.powered|((unsigned)bt.gated<<1)|((unsigned)bt.clocked<<2);
            native_bluetooth_stats[21]=uke_wcn_power_owners();
            for(unsigned i=0;i<4;i++)native_bluetooth_stats[22+i]=bt.geni.fault_irq[i];
            native_bluetooth_stats[26]=stats.malformed;native_bluetooth_stats[27]=stats.unmatched_events;
            native_bluetooth_stats[28]=bt.geni.stats.rx_dma;native_bluetooth_stats[29]=bt.geni.stats.tx_dma;
            native_bluetooth_stats[15]=bt.geni.stats.line_errors;native_bluetooth_stats[16]=bt.geni.stats.dma_errors;
            native_bluetooth_ibs[0]=stats.wake_ind;native_bluetooth_ibs[1]=stats.wake_ack;native_bluetooth_ibs[2]=stats.sleep_ind;
            native_bluetooth_ibs[3]=stats.clock_reverts;native_bluetooth_ibs[4]=stats.tx_refused;
            native_bluetooth_ibs[6]=bt.geni.stats.cancel_timeouts;
            health=t+1000000000ULL;
        }
        uint64_t deadline=bt.powered?qbt_deadline(bt.core):UINT64_MAX;
        uint64_t geni=bt.powered && !bt.gated?qbt_geni_deadline(&bt.geni):UINT64_MAX;
        if(geni<deadline)deadline=geni;if(health<deadline)deadline=health;if(closeAt && closeAt<deadline)deadline=closeAt;
        if(!deadline){IOSleep(1);continue;}
        waitUntil(nullptr,deadline);
    }
}
}
struct proc;
extern "C" {
void proc_iterate(unsigned,int (*)(struct proc *,void *),void *,int (*)(struct proc *,void *),void *);
int proc_pid(struct proc *);
void proc_name(int, char *, int);
}
int processSeen(struct proc *process,void *found) {
    static const char *const names[]={"bluetoothd","BlueTool"};
    char name[32] = {};
    proc_name(proc_pid(process), name, sizeof(name));
    for(unsigned i=0;i<2;i++)if(!strcmp(name,names[i]))static_cast<int *>(found)[i]=proc_pid(process);
    return 0;
}
void processWatch(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"uke-bluetooth-procs");
    for(unsigned sample=0;sample<60;sample++) {
        int found[2]={0,0};
        proc_iterate(1u|4u,processSeen,found,nullptr,nullptr);
        for(unsigned i=0;i<2;i++) {
            if(found[i] && (uint32_t)found[i]!=native_bluetooth_processes[i*2])native_bluetooth_processes[i*2+1]=native_bluetooth_processes[i*2+1]+1;
            native_bluetooth_processes[i*2]=(uint32_t)found[i];
        }
        if(found[0] || found[1])printf("[bluetooth proc] bluetoothd=%d starts=%u BlueTool=%d starts=%u\n",
            found[0], native_bluetooth_processes[1], found[1], native_bluetooth_processes[3]);
        IOSleep(2000);
    }
}
extern "C" void uke_bluetooth_probe(void) {
    unsigned enabled=0;
    if(!PE_parse_boot_argn("bluetooth",&enabled,sizeof(enabled)) || !enabled)return;
    uint32_t expected=0;if(!__atomic_compare_exchange_n(&bt.started,&expected,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    native_bluetooth_stats[0]=1;
    if(!address()){native_bluetooth_stats[0]=3;native_bluetooth_stats[1]=(uint64_t)(int64_t)-ENOENT;return;}
    PE_parse_boot_argn("log-bluetooth",&bt.log,sizeof(bt.log));
    if(bt.log>=2)native_bluetooth_boot_text=static_cast<char *>(IOMallocData(8192));
    thread_t thread=nullptr;
    if(kernel_thread_start(worker,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    else native_bluetooth_stats[0]=3;
    if(bt.log>=2 && kernel_thread_start(processWatch,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
}
