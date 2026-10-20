/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Software register/time peers only. No sheng MMIO/firmware is executed. */
#include "ShengGmu.h"
#include "ShengGmuSource.h"
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct {unsigned kind,window;size_t at;uint32_t value;} Event;
static struct {
    uint32_t *regs[5];Event events[4096];unsigned events_count,calls,fail_call,fail_after;
    unsigned waits,hold_firmware,hold_hfi,fail_reset,corrupt_payload,corrupt_config;
    unsigned stale_result,clock_stalled,clock_backwards,clock_jump,clock_fail,wait_fail;
    unsigned fault_on_start,release_count;
    unsigned protocol_peer,commands,protocol_fail,protocol_mode;
    uint64_t time;
} f;
static ShengGmuDevice device;static ShengGmuConfig config;
static uint8_t *firmware,*changed;static size_t firmware_bytes;
#ifdef SOURCE_ORACLE
extern void source_load_payloads(const uint8_t *,size_t,uint32_t *,uint32_t *);
#endif
static size_t offset(uint32_t word){return (size_t)word*4-SHENG_GMU_CORE_GPU_DELTA;}
static uint32_t *core(uint32_t word){return f.regs[0]+offset(word)/4;}
static int event(unsigned kind,unsigned window,size_t at,uint32_t value)
{
    CHECK(f.events_count<4096);f.events[f.events_count++]=(Event){kind,window,at,value};
    return ++f.calls==f.fail_call;
}
static int read32(void *cookie,unsigned window,size_t at,uint32_t *out)
{
    CHECK(cookie==&f && window<5 && !(at&3) && at+4<=sheng_gmu_window_bytes[window]);
    if(event(0,window,at,0))return 1;
    *out=f.regs[window][at/4];return 0;
}
static int protocol_response(void)
{
    volatile uint32_t *s=config.hfi.cpu;unsigned at=s[16];uint32_t header=s[1024+at];
    static const unsigned ids[]={4,3,11,7,11,14,10};
    static const unsigned words[]={59,160,4,37,4,2,1};
    CHECK(f.commands<7 && (header&255)==ids[f.commands] && ((header>>8)&255)==words[f.commands]);
    ++f.commands;s[16]=s[17];
    unsigned failed=f.commands==f.protocol_fail;
    if(failed) switch(f.protocol_mode) {
        case 1:return 1; /* notification may already have executed */
        case 2:return 0; /* no response */
        case 3:*core(REG_GMU_AO_HOST_INTERRUPT_STATUS)=1;break;
        case 4:*core(REG_GMU_GMU2HOST_INTR_INFO)=SHENG_GMU_CM3_FATAL_MASK;break;
        case 5:*core(REG_GMU_CM3_SYSRESET)=1;break;
        case 6:*core(REG_GMU_HFI_CTRL_STATUS)=0;break;
        case 7:f.clock_fail=1;break;
        case 8:f.clock_backwards=1;break;
        case 9:f.time+=10000;break;
        case 10:break; /* firmware error below */
        case 11:f.fail_call=f.calls+2;break; /* first health read, after notify barrier */
        default:CHECK(0);
    }
    at=s[29];s[2048+at]=(3u<<8)|126u;
    s[2048+((at+1)&1023)]=header;
    s[2048+((at+2)&1023)]=failed && f.protocol_mode==10?37:0;
    s[2048+((at+3)&1023)]=0xfafafafa;s[29]=(at+4)&1023;
    return 0;
}
static int write32(void *cookie,unsigned window,size_t at,uint32_t value)
{
    CHECK(cookie==&f && window<5 && !(at&3) && at+4<=sheng_gmu_window_bytes[window]);
    int failed=event(1,window,at,value);if(failed && !f.fail_after)return 1;
    if(window==0 && at==offset(REG_GMU_CM3_SYSRESET) && value==1 && f.release_count && f.fail_reset)return 1;
    if(window==0 && at==offset(REG_GMU_CM3_FW_INIT_RESULT) && !value && f.stale_result) {
        *core(REG_GMU_CM3_FW_INIT_RESULT)=0x100;return failed;
    }
    f.regs[window][at/4]=value;
    if(window==0 && at==offset(REG_GMU_CM3_ITCM_START) && f.corrupt_payload)f.regs[window][at/4]^=1;
    if(window==0 && at==offset(REG_GMU_HFI_QTBL_ADDR) && f.corrupt_config)f.regs[window][at/4]^=4096;
    if(window==0 && at==offset(REG_GMU_CM3_SYSRESET) && !value) {
        ++f.release_count;
        if(!f.hold_firmware)*core(REG_GMU_CM3_FW_INIT_RESULT)=0x100;
        if(f.fault_on_start)*core(REG_GMU_AO_HOST_INTERRUPT_STATUS)=1;
    }
    if(window==0 && at==offset(REG_GMU_HFI_CTRL_INIT) && value && !f.hold_hfi)*core(REG_GMU_HFI_CTRL_STATUS)=1;
    if(window==0 && at==offset(REG_GMU_HOST2GMU_INTR_SET) && value==1 && f.protocol_peer && protocol_response())return 1;
    return failed;
}
static int barrier(void *cookie){CHECK(cookie==&f);return event(2,0,0,0);}
static int hash(void *cookie,const void *input,size_t bytes,uint8_t digest[32])
{CHECK(cookie==&f && bytes<=UINT32_MAX);return CC_SHA256(input,(CC_LONG)bytes,digest)?0:1;}
static int now(void *cookie,uint64_t *value)
{
    CHECK(cookie==&f);if(f.clock_fail)return 1;
    *value=f.time;
    if(f.clock_backwards)f.time--;
    if(f.clock_jump)f.time+=10000;
    return 0;
}
static int wait_us(void *cookie,unsigned us)
{CHECK(cookie==&f && us==100);++f.waits;if(f.wait_fail)return 1;if(!f.clock_stalled)f.time+=us;return 0;}
static void *allocate(size_t bytes)
{void *p=NULL;CHECK(!posix_memalign(&p,4096,bytes));memset(p,0,bytes);return p;}
static void reset(void)
{
    for(unsigned i=0;i<5;++i)memset(f.regs[i],0,sheng_gmu_window_bytes[i]);
    f.events_count=f.calls=f.fail_call=f.fail_after=f.waits=0;
    f.hold_firmware=f.hold_hfi=f.fail_reset=f.corrupt_payload=f.corrupt_config=f.stale_result=0;
    f.clock_stalled=f.clock_backwards=f.clock_jump=f.clock_fail=f.wait_fail=f.fault_on_start=f.release_count=0;f.time=1000000;
    f.protocol_peer=f.commands=f.protocol_fail=f.protocol_mode=0;
    *core(REG_GMU_CM3_SYSRESET)=1;
    device=(ShengGmuDevice){0};device.io=(ShengGmuIo){&f,{0},read32,write32,barrier};
    for(unsigned i=0;i<5;++i)device.io.bytes[i]=sheng_gmu_window_bytes[i];
    device.services=(ShengGmuServices){&f,hash,now,wait_us};config.firmware=firmware;config.firmware_bytes=firmware_bytes;config.poll_budget=4;
}
static void no_writes(void)
{for(unsigned i=0;i<f.events_count;++i)CHECK(f.events[i].kind!=1);}
static unsigned find_write(unsigned window,size_t at,uint32_t value)
{
    for(unsigned i=0;i<f.events_count;++i)if(f.events[i].kind==1 && f.events[i].window==window &&
        f.events[i].at==at && f.events[i].value==value)return i+1;
    CHECK(0);return 0;
}
static void adapter(void)
{
    ShengGmuMmio m={0};ShengGmuIo io;
    for(unsigned i=0;i<5;++i)m.windows[i]=(ShengGmuMmioWindow){(volatile unsigned char *)f.regs[i],sheng_gmu_window_physical[i],sheng_gmu_window_bytes[i]};
    CHECK(ShengGmuBindMmio(&m,&io)==ShengGmuSuccess);uint32_t v=0;
    CHECK(!io.write32(io.cookie,0,4,0x12345678) && !io.read32(io.cookie,0,4,&v) && v==0x12345678 && !io.barrier(io.cookie));
    CHECK(io.read32(io.cookie,0,1,&v) && io.read32(io.cookie,5,0,&v) && io.write32(io.cookie,0,SIZE_MAX-3,1));
    m.windows[4].physical=0x03d61000; /* cx_dbgc must not become cx_mem. */
    CHECK(ShengGmuBindMmio(&m,&io)==ShengGmuArgument && !io.read32);
    m.windows[4].physical=sheng_gmu_window_physical[4];m.windows[4].cpu=m.windows[0].cpu;
    CHECK(ShengGmuBindMmio(&m,&io)==ShengGmuArgument);
}
extern void test_sheng_votes(AdrenoHfiStartupConfig *);
extern void test_sheng_command_db(AdrenoHfiStartupConfig *);
static void cold_start(void)
{
    /* Deliberately synthetic votes/addresses: this fixture is never deployed. */
    AdrenoHfiStartupConfig c={.gpu_count=2,.gmu_count=2,.gpu={{0,0},{100000000,1}},
        .gmu={{0,0},{200000000,1}},.bandwidth_count=1,.cnoc_count=1,.ddr_count=1,
        .cnoc_addresses={0x1000},.ddr_addresses={0x2000},.acd_version=1,
        .acd_enable_mask=2,.acd_stride=1,.acd_count=1,.acd_data={41},.enable_ifpc=1};
    ShengGmuStartupLimits limits={4,100,1000,10000};ShengGmuStartupResult r;
    reset();f.protocol_peer=1;
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuSuccess);
    CHECK(f.commands==7 && f.release_count==1 && device.startup.completed && !device.poisoned &&
        r.protocol.stage==AdrenoHfiStageComplete && r.protocol.published_commands==7 &&
        r.protocol_status==AdrenoHfiStartupSuccess && r.load.hfi_ready_observed && !*core(REG_GMU_CM3_SYSRESET));
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuBusy && f.commands==7);
    for(unsigned fail=1;fail<=7;++fail)for(unsigned mode=1;mode<=11;++mode) {
        reset();f.protocol_peer=1;f.protocol_fail=fail;f.protocol_mode=mode;
        ShengGmuStatus expected=ShengGmuProtocolError;
        if(mode==2 || mode==9)expected=ShengGmuTimeout;
        if(mode==3 || mode==4)expected=ShengGmuFault;
        if(mode==5 || mode==6)expected=ShengGmuStateMismatch;
        if(mode==7 || mode==8)expected=ShengGmuClockError;
        if(mode==11)expected=ShengGmuIoError;
        CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==expected);
        CHECK(f.commands==fail && f.release_count==1 && device.poisoned && device.startup.poisoned &&
            device.hfi.poisoned && !device.startup.completed && r.load.reset_verified &&
            r.load.resources_must_retain && r.protocol.resources_must_retain && *core(REG_GMU_CM3_SYSRESET)==1);
        unsigned calls=f.calls;
        CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuPoisoned && f.calls==calls);
    }
    reset();f.protocol_peer=1;f.protocol_fail=7;f.protocol_mode=2;f.fail_reset=1;
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuTimeout && !r.load.reset_verified &&
        r.load.resources_must_retain && r.protocol.resources_must_retain && device.poisoned);
    reset();c.cnoc_addresses[0]=0;
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuArgument && !f.events_count);
    c.cnoc_addresses[0]=0x1000;limits.polls_per_command=0;
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuArgument && !f.events_count);
    limits.polls_per_command=4;device.startup.poisoned=1;
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuPoisoned && !f.events_count);
    reset();config.firmware=changed;
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuFirmware &&
        r.protocol_status==AdrenoHfiStartupInvalid && !f.events_count && !device.startup.attempted);
    test_sheng_votes(&c);reset();f.protocol_peer=1;
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuSuccess && f.commands==7 && device.startup.completed);
    test_sheng_command_db(&c);reset();f.protocol_peer=1;
    CHECK(ShengGmuColdStart(&device,&config,&c,&limits,&r)==ShengGmuSuccess && f.commands==7 && device.startup.completed);
    puts("PASS: combined cold load + seven ACK stages, 77 stage-specific fault controls, retained resources and no replay");
}
int main(int argc,char **argv)
{
    CHECK(argc==2);FILE *in=fopen(argv[1],"rb");CHECK(in && !fseek(in,0,SEEK_END));long size=ftell(in);CHECK(size>0 && !fseek(in,0,SEEK_SET));
    firmware_bytes=(size_t)size;firmware=malloc(firmware_bytes);changed=malloc(firmware_bytes);CHECK(firmware && changed);
    CHECK(fread(firmware,1,firmware_bytes,in)==firmware_bytes && !fclose(in));memcpy(changed,firmware,firmware_bytes);changed[firmware_bytes-1]^=1;
    for(unsigned i=0;i<5;++i)f.regs[i]=allocate(sheng_gmu_window_bytes[i]);
    config=(ShengGmuConfig){.chip_id=SHENG_GMU_CHIP_ID,
        .icache={allocate(0xffc000),0xffc000,0x4000},.dummy={allocate(0x4000),0x4000,0x60000000},
        .debug={allocate(0x7000),0x7000,0x60400000},.log={allocate(0x4000),0x4000,0x61000000},
        .hfi={allocate(0x4000),0x4000,0x61004000}};
    reset();ShengGmuResult r;
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuSuccess);
    CHECK(r.phase==7 && r.complete_hash_verified && r.firmware_ready_observed && r.hfi_ready_observed && !r.reset_verified &&
        r.resources_must_retain && !device.poisoned && device.boot_attempted && f.release_count==1);
    CHECK(*core(REG_GMU_GENERAL_10)==0x7020100 && *core(REG_GMU_HFI_QTBL_ADDR)==config.hfi.iova);
    unsigned writes=0;
    for(unsigned i=0;i<f.events_count && writes<SHENG_GMU_RSC_SETUP_COUNT;++i)if(f.events[i].kind==1) {
        const ShengGmuSetupWrite *w=sheng_gmu_rsc_setup+writes++;
        CHECK(f.events[i].window==w->window && f.events[i].at==w->byte_offset && f.events[i].value==w->value);
    }
    CHECK(writes==19);
    CHECK(find_write(ShengGmuCxMisc,REG_CX_MISC_TCM_RET_CNTL*4,1)<find_write(ShengGmuCore,offset(REG_GMU_CM3_ITCM_START),*(const uint32_t *)(firmware+16*12)));
#ifdef SOURCE_ORACLE
    uint32_t *oracle_core=allocate(sheng_gmu_window_bytes[0]),*oracle_cache=allocate(config.icache.bytes);
    source_load_payloads(firmware,firmware_bytes,oracle_core,oracle_cache);
    CHECK(!memcmp(oracle_core+offset(REG_GMU_CM3_ITCM_START)/4,f.regs[0]+offset(REG_GMU_CM3_ITCM_START)/4,0x8000));
    CHECK(!memcmp(oracle_cache,(const void *)config.icache.cpu,config.icache.bytes));free(oracle_core);free(oracle_cache);
#endif
    /* Sample distinct real mutation boundaries, including writes whose reported
     * failure may occur after the device side effect. Never execute a device. */
    unsigned fail_points[12];
    fail_points[0]=find_write(1,sheng_gmu_rsc_setup[0].byte_offset,sheng_gmu_rsc_setup[0].value);
    fail_points[1]=find_write(2,REG_PDC_GPU_ENABLE_PDC*4,0x80000001);
    fail_points[2]=find_write(4,REG_CX_MISC_TCM_RET_CNTL*4,1);
    fail_points[3]=find_write(0,offset(REG_GMU_CM3_FW_INIT_RESULT),0);
    fail_points[4]=find_write(0,offset(REG_GMU_HFI_QTBL_ADDR),(uint32_t)config.hfi.iova);
    fail_points[5]=find_write(0,offset(REG_GMU_CM3_CFG),0x4052);
    fail_points[6]=find_write(0,offset(REG_GMU_GENERAL_10),0x7020100);
    fail_points[7]=find_write(0,offset(REG_GMU_SYS_BUS_CONFIG),1);
    fail_points[8]=find_write(0,offset(REG_GMU_GENERAL_9),0);
    fail_points[9]=find_write(0,offset(REG_GMU_CM3_SYSRESET),0);
    fail_points[10]=find_write(0,offset(REG_GMU_HFI_CTRL_INIT),1);
    fail_points[11]=f.events_count; /* final fault-status/barrier observation */
    unsigned normal_calls=f.calls;
    CHECK(AdrenoHfiSend(&device.hfi,14,NULL,0,&writes)==AdrenoHfiSuccess);
    CHECK(*core(REG_GMU_HOST2GMU_INTR_SET)==1); /* real adapter path on fixture */
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuBusy);
    for(unsigned after=0;after<2;++after)for(unsigned i=0;i<12;++i) {
        reset();f.fail_call=fail_points[i];f.fail_after=after;
        CHECK(f.fail_call<=normal_calls);
        CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuIoError);
        CHECK(device.poisoned && device.hfi.poisoned && r.reset_verified && r.resources_must_retain);
        CHECK(*core(REG_GMU_CM3_SYSRESET)==1);
    }
    reset();config.firmware=changed;
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuFirmware && !f.events_count);no_writes();
    reset();config.firmware_bytes--;
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuArgument && !f.events_count);
    reset();*core(REG_GMU_CM3_SYSRESET)=0;
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuBusy);no_writes();
    reset();*core(REG_GMU_HFI_CTRL_STATUS)=1;
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuStateMismatch);no_writes();
    reset();f.regs[4][REG_CX_MISC_TCM_RET_CNTL]=1;
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuStateMismatch);no_writes();
    for(unsigned control=0;control<11;++control) {
        reset();ShengGmuStatus expected=ShengGmuTimeout;
        switch(control) {
            case 0:f.hold_firmware=1;break;
            case 1:f.hold_hfi=1;break;
            case 2:f.hold_firmware=1;f.clock_stalled=1;break;
            case 3:f.hold_firmware=1;f.clock_backwards=1;expected=ShengGmuClockError;break;
            case 4:f.hold_firmware=1;f.clock_jump=1;break;
            case 5:f.clock_fail=1;expected=ShengGmuClockError;break;
            case 6:f.hold_firmware=1;f.wait_fail=1;expected=ShengGmuClockError;break;
            case 7:f.corrupt_payload=1;expected=ShengGmuStateMismatch;break;
            case 8:f.corrupt_config=1;expected=ShengGmuStateMismatch;break;
            case 9:f.stale_result=1;expected=ShengGmuStateMismatch;break;
            case 10:f.fault_on_start=1;expected=ShengGmuFault;break;
        }
        CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==expected);
        CHECK(device.poisoned && r.reset_verified && r.resources_must_retain && !r.hfi_ready_observed);
        if(control>=7 && control<=9)CHECK(!f.release_count);
        CHECK(f.waits<=3);
    }
    reset();f.hold_hfi=1;f.fail_reset=1;
    CHECK(ShengGmuLoadAndStartHfi(&device,&config,&r)==ShengGmuTimeout);
    CHECK(!r.reset_verified && r.resources_must_retain && device.poisoned && !*core(REG_GMU_CM3_SYSRESET));
    cold_start();
    adapter();
    for(unsigned i=0;i<5;++i)free(f.regs[i]);
    free((void *)config.icache.cpu);free((void *)config.dummy.cpu);free((void *)config.debug.cpu);free((void *)config.log.cpu);free((void *)config.hfi.cpu);free(firmware);free(changed);
    puts("PASS: pinned actual GMU image, full payload copy/readback, cold-reset/retention ownership guards, RSC/PDC/register ordering, 24 I/O failure controls, fresh readiness and bounded fault/timeout quarantine");
    puts("Software register/time peers only; no sheng MMIO, actual GMU execution, active SMMU mapping or GPU submission");
}
