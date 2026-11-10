// SPDX-License-Identifier: BSD-2-Clause-Patent
// ADSP transport from UniXNU-sheng 6bc0ce1e, with the uke Cliffs PAS layout.
// adsp=1 surveys the firmware handoff; adsp=2 starts the stock DSP image.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>
#include <libkern/crypto/sha2.h>
#include "Sm7675RemoteProc.h"
extern "C" {
#include "QcomGlink.h"
#include "QcomMdt.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"
#include "ShengAdsp.h"
#include "UkeWpssMemory.h"
void sheng_qrtr_start(void);
int sheng_fw_open(const char *path, void **handle, uint64_t *size);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);
}

// [0] stage (see Stage; negative: the step that failed), [1] last error, [2] IPCC interrupts, [3] the DSP's slave-kernel bits (-1 no
// item), [4] items (bit 0 SMP2P in 443, 1 out 429, 2 GLINK 478, 3 479, 4 480, 8 our partition, 9 global partition), [5] QMP state | AOSS
// link bits << 8, [6] RSC DRV id, [7] adsp.mbn KiB, [8] ms from the start call to "ready", [9] GLINK messages in, [10] out, [11] channels
// up (bit per client) | version agreed << 14 | link broken << 15, [12] channel opens nobody serves, [13] last SCM x0, [14] its x1,
// [15] votes: lcx index | lmx index << 8 | RSC done interrupts << 16
extern "C" {volatile int32_t native_adsp_stats[16]; char native_adsp_text[96];}

namespace {
enum Stage {
    StMapped = 1, StSurveyed = 2, StFirmware = 3, StSmp2p = 4, StDoorbells = 5, StLoadState = 6, StRails = 7, StDtb = 8, StLoaded = 9,
    StStarted = 10, StReady = 11, StGlink = 12, StChannel = 13,
};
constexpr uint64_t kSmem = 0x81d00000, kSmemSize = 0x200000, kMutex = 0x1f40000 + 3 * 0x1000, kAoss = 0xc300000;
constexpr uint64_t kRscDrv = 0x17a20000, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;
constexpr uint64_t kAdspBase = 0x9d400000, kAdspSize = 0x4500000, kDtbBase = 0xa1900000, kDtbSize = 0x80000;
constexpr uint32_t kPasAdsp = 1, kPasDtb = 36, kRemotePid = 2, kItemIn = 443, kItemOut = 429, kItemCrash = 423;
constexpr uint32_t kItemGlinkDesc = 478, kItemGlinkTx = 479, kItemGlinkRx = 480, kGlinkFifo = 0x4000;
constexpr uint32_t kFatal = 1u << 0, kReady = 1u << 1, kHandover = 1u << 2;
constexpr uint32_t kEvSmp2p = 1, kEvGlink = 2, kEvSend = 16;
constexpr unsigned kClients = 4, kQueue = 8;

struct Window {IOMemoryMap *map; volatile uint8_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
uint32_t rd(const Window &w, uint64_t at) {return *reinterpret_cast<volatile uint32_t *>(w.va + at);}
void fail(int step, int error) {
    native_adsp_stats[1] = error;
    native_adsp_stats[0] = -step;
    printf("UKE ADSP: failed stage=%d error=%d scm=%d/%d %s\n", step, error,
           native_adsp_stats[13], native_adsp_stats[14], native_adsp_text);
}
void stage(int value) {
    native_adsp_stats[0] = value;
    printf("UKE ADSP: stage=%d slave=%x items=%x\n", value,
           (unsigned)native_adsp_stats[3], (unsigned)native_adsp_stats[4]);
}
void note(const char *prefix, const char *text) {
    size_t at = 0;
    for (; prefix[at] && at < sizeof native_adsp_text - 1; ++at) native_adsp_text[at] = prefix[at];
    for (size_t i = 0; text[i] && at < sizeof native_adsp_text - 1; ++i) native_adsp_text[at++] = text[i];
    native_adsp_text[at] = 0;
}

Window gSmemW, gMutex, gAoss, gRsc;
QcomSmem gSmem;
QcomSmemPartition gPart;
QcomSmp2p gP2p;
int gStopEntry = -1;
QcomGlink gGlink;
bool gGlinkStarted;
bool gProxyHeld, gLoadStateOn;
volatile uint32_t gPending;

// ---- clients ----
struct Message {uint32_t length; uint8_t data[SHENG_ADSP_MAX_MESSAGE];};
}  // namespace
extern "C" {volatile int32_t native_adsp_audio[8];}
struct ShengAdspChannel {
    char name[QCOM_GLINK_NAME];
    uint32_t intent_size, intent_count;
    int initiator;
    ShengAdspClient client;
    void *context;
    uint8_t *storage;
    QcomGlinkChannel *glink;
    Message *queue;  // kQueue slots
    uint32_t head, count;
    bool up;
};
namespace {
ShengAdspChannel gClients[kClients];
uint32_t gClientCount;
uint32_t gGeneration;
bool gTransportFailed;
IOLock *gClientLock;

void wake(uint32_t bits) {
    __atomic_fetch_or(&gPending, bits, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}

// ---- interrupts: acknowledge, record, wake ----
void ipccDoorbell(uint32_t signal) {
    native_adsp_stats[2] = native_adsp_stats[2] + 1;
    wake(signal == QCOM_IPCC_SIGNAL_SMP2P ? kEvSmp2p : kEvGlink);
}

// Waits for an event up to ms milliseconds; returns the event bits (0: the deadline passed).
uint32_t waitEvents(uint32_t ms) {
    uint32_t bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
    if (bits) return bits;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms, kMillisecondScale);
    bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
    if (bits) {clear_wait(current_thread(), THREAD_AWAKENED); return bits;}
    thread_block(THREAD_CONTINUE_NULL);
    return __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
}

constexpr auto kRemote = Sm7675Remote::Adsp;
void kickGlink(void *) {sm7675_remote_ring(kRemote, QCOM_IPCC_SIGNAL_GLINK_QMP);}
void kickSmp2p() {sm7675_remote_ring(kRemote, QCOM_IPCC_SIGNAL_SMP2P);}

// ---- SMEM helpers (allocation under the TCSR hardware mutex) ----
int allocItem(uint16_t item, uint32_t size) {
    const int rc = sm7675_remote_smem_alloc(gMutex.va, &gSmem, &gPart, item, size);
    return rc == QcomSmemExists ? 0 : rc;
}
volatile uint8_t *item(uint16_t id, uint32_t *size) {
    uint32_t at = 0, bytes = 0;
    if (qcom_smem_find(&gSmem, &gPart, id, &at, &bytes)) return nullptr;
    if (size) *size = bytes;
    return gSmemW.va + at;
}

void readCrashReason() {
    QcomSmemPartition global;
    uint32_t at = 0, size = 0;
    if (qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global) ||
        qcom_smem_find(&gSmem, &global, kItemCrash, &at, &size) || !size)
        return;
    char text[80];
    size_t n = 0;
    for (; n < sizeof text - 1 && n < size; ++n) {const char c = (char)gSmemW.va[at + n]; if (!c) break; text[n] = c >= 32 && c < 127 ? c : '.';}
    text[n] = 0;
    if (n) note("crash: ", text);
}

// ---- the survey (adsp=1 and the first step of 2): reads only ----
void survey() {
    int32_t bits = 0;
    if (!qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_APPS, kRemotePid, &gPart)) {
        bits |= 1 << 8;
        const uint16_t ids[5] = {kItemIn, kItemOut, kItemGlinkDesc, kItemGlinkTx, kItemGlinkRx};
        for (unsigned i = 0; i < 5; ++i) if (item(ids[i], nullptr)) bits |= 1 << i;
    }
    QcomSmemPartition global;
    if (!qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global)) bits |= 1 << 9;
    native_adsp_stats[4] = bits;
    native_adsp_stats[3] = -1;
    if (volatile uint8_t *in = item(kItemIn, nullptr)) {
        QcomSmp2p view = {};
        view.in = reinterpret_cast<volatile uint32_t *>(in);
        view.local_pid = 0; view.remote_pid = kRemotePid;
        const int index = qcom_smp2p_check_in(view.in, 0, kRemotePid) ? -1 : qcom_smp2p_find_in(&view, "slave-kernel");
        if (index >= 0) native_adsp_stats[3] = (int32_t)qcom_smp2p_in_value(&view, index);
    }
    const uint32_t up = 0xffff;
    native_adsp_stats[5] = (rd(gAoss, 0) == 0x4d41494cu ? 1 : 0) << 12 | (rd(gAoss, 0xc) == up) << 8 | (rd(gAoss, 0x24) == up) << 9 |
                           (rd(gAoss, 0x14) == up) << 10 | (rd(gAoss, 0x2c) == up) << 11;
    native_adsp_stats[6] = (int32_t)rd(gRsc, 0);
    readCrashReason();
}

void service(uint32_t bits);

// ---- RPMh proxy votes ----
bool readCmdDb(uint8_t *copy) {
    Window db = {};
    if (!mapWindow(db, kCmdDb, kCmdDbSize)) return false;
    for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = rd(db, i); memcpy(copy + i, &w, 4);}
    unmap(db);
    return true;
}
QcomRpmhResource gLcx, gLmx, gXo;
int vote(uint32_t lcx, uint32_t lmx) {
    const Sm7675RpmhVote votes[] = {{gLcx.address, lcx}, {gLmx.address, lmx}, {gXo.address, 3}};
    const int rc = sm7675_remote_vote(kRemote, votes, lcx || lmx ? 3 : 0);
    native_adsp_stats[15] = (int32_t)(lcx | lmx << 8 | (rc ? 1u << 15 : 0));
    if (!rc) gProxyHeld = lcx || lmx;
    return rc;
}

// ---- secure world calls ----
struct ScmResult {int64_t x0; uint64_t x1;};
ScmResult scm(uint32_t command, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c) {
    ScmResult result = {-1, 0};
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t function = qcom_scm_fn(QCOM_SCM_SVC_PIL, command), session = 0;
        unsigned resumes = 0;
        for (;;) {
            register uint64_t x0 __asm__("x0") = function, x1 __asm__("x1") = arginfo, x2 __asm__("x2") = a, x3 __asm__("x3") = b,
                              x4 __asm__("x4") = c, x5 __asm__("x5") = 0, x6 __asm__("x6") = session, x7 __asm__("x7") = 0;
            __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                             :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
            result = {(int64_t)x0, x1};
            if ((int64_t)x0 != QCOM_SCM_INTERRUPTED || ++resumes > 100000) break;
            function = QCOM_SCM_INTERRUPTED;  // resume the interrupted call in its session
            session = x6;
        }
        if (result.x0 != QCOM_SCM_V2_EBUSY) break;
        IOSleep(30);
    }
    native_adsp_stats[13] = (int32_t)result.x0;
    native_adsp_stats[14] = (int32_t)result.x1;
    return result;
}
bool scmOk(const ScmResult &r) {return r.x0 == 0 && r.x1 == 0;}

// ---- a firmware image: parsed from its file, metadata handed over, segments written into the carveout ----
struct Image {  // plain data, zero at start: the two live in static storage (a few KiB of header table each, kept off the stack)
    void *file;
    uint64_t size;
    QcomMdtImage mdt;
    IOBufferMemoryDescriptor *meta;
    bool prepared;
    bool secureMetadata;
    uint32_t pas;
    bool pasActive;
};
Image gDtbImage, gAdspImage;
int fileRead(void *context, uint64_t offset, void *to, size_t length) {return sheng_fw_read(context, offset, to, length);}
void release(Image &image) {
    if (image.file) sheng_fw_close(image.file);
    image.file = nullptr;
    if (image.meta && image.secureMetadata) {
        printf("UKE ADSP: retain metadata pas=%u until shutdown is confirmed\n", image.pas);
        return;
    }
    if (image.meta) {
        if (image.prepared) image.meta->complete();
        image.meta->release();
    }
    image.prepared = false;
    image.file = nullptr; image.meta = nullptr;
}
int openImage(Image &image, const char *path) {
    int rc = sheng_fw_open(path, &image.file, &image.size);
    if (rc) {image.file = nullptr; return rc;}
    rc = qcom_mdt_parse(&image.mdt, image.size, fileRead, image.file);
    if (!rc) printf("UKE ADSP: source=%s virtual_bytes=%llu segments=%u\n",
                    path, (unsigned long long)image.size, image.mdt.count);
    return rc ? 1000 - rc : 0;
}
// init_image with the metadata in physically contiguous memory, then the memory setup of a relocatable image.
int authenticateHeader(Image &image, uint32_t pas, uint64_t base) {
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    if (!bytes || bytes > UINT64_MAX - 0xfff) return -7;
    const uint64_t allocation = (bytes + 0xfff) & ~0xfffull;
    // Uke OEM mdt_loader uses a page-aligned, non-cacheable coherent buffer:
    // secure authentication can protect its entire physical pages (XPU).
    // Write-combine selects normal uncached memory, retaining bulk-copy support.
    image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous |
        (IOOptionBits)kIOMapWriteCombineCache, allocation, 0xfffffffffffff000ull);
    if (!image.meta || image.meta->prepare() != kIOReturnSuccess) return -12;
    image.prepared = true;
    uint8_t *va = static_cast<uint8_t *>(image.meta->getBytesNoCopy());
    if (qcom_mdt_metadata(&image.mdt, fileRead, image.file, va, bytes)) return -5;
    uint8_t digest[32];
    SHA256_CTX hash;
    SHA256_Init(&hash);
    SHA256_Update(&hash, va, bytes);
    SHA256_Final(digest, &hash);
    char hex[65];
    for (unsigned i = 0; i < sizeof digest; ++i)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    printf("UKE ADSP: pas=%u base=%llx metadata=%llu sha256=%s\n",
           pas, (unsigned long long)base, (unsigned long long)bytes, hex);
    __asm__ volatile("dsb sy" ::: "memory");
    IOByteCount segment = 0;
    const uint64_t phys = image.meta->getPhysicalSegment(0, &segment, kIOMemoryMapperNone);
    if (!phys || (phys & 0xfff) || segment < allocation) return -12;
    printf("UKE ADSP: metadata pas=%u pa=%llx span=%llu allocation=%llu cache=normal-uncached\n",
        pas, (unsigned long long)phys, (unsigned long long)segment, (unsigned long long)allocation);
    // Even a failed INIT_IMAGE may have enabled memory protection. Keep the
    // buffer owned until a successful shutdown on every failure path.
    image.pas = pas;
    image.secureMetadata = true;
    image.pasActive = true;
    if (!scmOk(scm(QCOM_SCM_PAS_INIT_IMAGE, qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0), pas, phys, 0))) return -1;
    if (image.mdt.relocatable &&
        !scmOk(scm(QCOM_SCM_PAS_MEM_SETUP, qcom_scm_args(3, 0, 0, 0), pas, base, qcom_mdt_span(&image.mdt))))
        return -2;
    return 0;
}
int loadSegments(Image &image, uint64_t base, uint64_t size) {
    Window region = {};
    if (!mapWindow(region, base, size, kIOMapWriteCombineCache)) return -12;
    constexpr size_t kChunk = 256 * 1024;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kChunk));
    int rc = bounce ? 0 : -12;
    for (unsigned i = 0; !rc && i < image.mdt.count; ++i) {
        const QcomMdtSegment &s = image.mdt.segment[i];
        uint64_t at = 0;
        if (!s.load) continue;
        if (qcom_mdt_place(&image.mdt, i, base, size, &at)) {rc = -7; break;}
        uint8_t *to = const_cast<uint8_t *>(region.va) + at;
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (sheng_fw_read(image.file, s.file_offset + done, bounce, n)) {rc = -5; break;}
            uke_wpss_memory_copy(to + done, bounce, n);
            done += n;
        }
        if (!rc && s.memory_size > s.file_size) uke_wpss_memory_zero(to + s.file_size, (size_t)(s.memory_size - s.file_size));
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (bounce) IOFreeData(bounce, kChunk);
    unmap(region);
    return rc;
}

// ---- events ----
uint32_t gSlave;
bool gHandedOver;
void disconnectClients();
void serviceSmp2p() {
    if (!gP2p.in) {
        if (volatile uint8_t *in = item(kItemIn, nullptr)) gP2p.in = reinterpret_cast<volatile uint32_t *>(in);
        else return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gP2p, "slave-kernel", &value, &changed)) kickSmp2p();
    gSlave = value;
    native_adsp_stats[3] = (int32_t)value;
    if ((changed & value & kFatal)) {
        readCrashReason();
        if (gGlinkStarted) {
            disconnectClients();
            gGlinkStarted = false;
            __atomic_store_n(&gTransportFailed, true, __ATOMIC_RELEASE);
            fail(StReady, -5);
        }
    }
    if ((value & kHandover) && !gHandedOver) {
        gHandedOver = true;
        vote(0, 0);  // the DSP holds its own votes now
    }
}

void applyClients();
void pumpSends();
void serviceGlink() {
    if (!gGlinkStarted) return;
    if (!gGlink.rx.data) {
        uint32_t size = 0;
        if (volatile uint8_t *rx = item(kItemGlinkRx, &size)) qcom_glink_set_rx(&gGlink, rx, size);
    }
    if (qcom_glink_poll(&gGlink) < 0) {
        disconnectClients();
        gGlinkStarted = false;
        __atomic_store_n(&gTransportFailed, true, __ATOMIC_RELEASE);
        fail(StGlink, -5);
        return;
    }
    pumpSends();
    uint32_t up = 0;
    for (unsigned i = 0; i < __atomic_load_n(&gClientCount, __ATOMIC_ACQUIRE); ++i) if (gClients[i].glink && gClients[i].glink->up) up |= 1u << i;
    native_adsp_stats[9] = (int32_t)gGlink.rx_messages;
    native_adsp_stats[10] = (int32_t)gGlink.tx_messages;
    native_adsp_stats[11] = (int32_t)(up | (uint32_t)gGlink.version_agreed << 14 | (uint32_t)gGlink.broken << 15);
    native_adsp_stats[12] = (int32_t)gGlink.unknown_opens;
    if (gGlink.unknown_opens && strncmp(native_adsp_text, "crash: ", 7)) note("open: ", gGlink.unknown_names);
    if (gGlink.version_agreed && native_adsp_stats[0] >= StReady && native_adsp_stats[0] < StGlink) stage(StGlink);
    if (up && native_adsp_stats[0] == StGlink) stage(StChannel);
}
void service(uint32_t bits) {
    if (bits & (kEvSmp2p | kEvGlink)) serviceSmp2p();
    if (bits & (kEvGlink | kEvSend)) {applyClients(); serviceGlink();}
}
// Runs events until done() holds or ms pass; returns done().
template <typename F> bool waitUntil(uint32_t ms, F done) {
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    while (!done()) {
        const uint64_t now = mach_absolute_time();
        if (now >= deadline) return done();
        uint64_t left = 0;
        absolutetime_to_nanoseconds(deadline - now, &left);
        service(waitEvents((uint32_t)(left / 1000000) + 1));
    }
    return true;
}

// ---- client glue (DSP thread side) ----
void clientState(void *context, QcomGlinkChannel *, int up) {
    ShengAdspChannel *c = static_cast<ShengAdspChannel *>(context);
    IOLockLock(gClientLock);
    c->up = up != 0;
    if (!up) c->head = c->count = 0;
    IOLockUnlock(gClientLock);
    printf("UKE ADSP: channel=%s up=%d generation=%u\n", c->name, up, gGeneration);
    if (c->client.state) c->client.state(c->context, up);
}
void clientReceive(void *context, QcomGlinkChannel *, const uint8_t *data, uint32_t length) {
    ShengAdspChannel *c = static_cast<ShengAdspChannel *>(context);
    if (c->client.receive) c->client.receive(c->context, data, length);
}
void disconnectClients() {
    for (unsigned i = 0; i < __atomic_load_n(&gClientCount, __ATOMIC_ACQUIRE); ++i)
        clientState(&gClients[i], nullptr, 0);
}
void applyClients() {
    if (!gGlinkStarted) return;
    const QcomGlinkClient glue = {clientState, clientReceive};
    IOLockLock(gClientLock);
    for (unsigned i = 0; i < __atomic_load_n(&gClientCount, __ATOMIC_ACQUIRE); ++i) {
        ShengAdspChannel &c = gClients[i];
        if (!c.glink) c.glink = qcom_glink_channel(&gGlink, c.name, c.intent_size, c.intent_count, c.storage, &glue, &c, c.initiator);
    }
    IOLockUnlock(gClientLock);
}
// The audio channel's send state, for the log: [0] sent, [1] deferred (no intent or FIFO room), [2] refused otherwise, [3] queued now,
// [4] intent asked (0/1), [5] valid remote intents, [6] the largest remote intent, [7] the last deferred length
void noteChannel(const ShengAdspChannel &c, int rc, uint32_t length) {
    if (strcmp(c.name, "adsp_apps")) return;
    if (rc == QcomGlinkOk) native_adsp_audio[0] = native_adsp_audio[0] + 1;
    else if (rc == QcomGlinkAgain) {native_adsp_audio[1] = native_adsp_audio[1] + 1; native_adsp_audio[7] = (int32_t)length;}
    else native_adsp_audio[2] = native_adsp_audio[2] + 1;
    native_adsp_audio[3] = (int32_t)c.count;
    native_adsp_audio[4] = c.glink->intent_asked;
    int32_t valid = 0, largest = 0;
    for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k)
        if (c.glink->remote[k].valid) {++valid; if ((int32_t)c.glink->remote[k].size > largest) largest = (int32_t)c.glink->remote[k].size;}
    native_adsp_audio[5] = valid;
    native_adsp_audio[6] = largest;
}
void pumpSends() {
    for (unsigned i = 0; i < __atomic_load_n(&gClientCount, __ATOMIC_ACQUIRE); ++i) {
        ShengAdspChannel &c = gClients[i];
        if (!c.glink || !c.glink->up) continue;
        for (;;) {
            IOLockLock(gClientLock);
            if (!c.count) {IOLockUnlock(gClientLock); break;}
            Message *m = &c.queue[c.head];
            IOLockUnlock(gClientLock);
            // Only this thread takes messages off the queue, so m stays valid while the lock is dropped for the send.
            const int rc = qcom_glink_send(&gGlink, c.glink, m->data, m->length);
            noteChannel(c, rc, m->length);
            if (rc != QcomGlinkOk) break;
            IOLockLock(gClientLock);
            c.head = (c.head + 1) % kQueue;
            c.count--;
            IOLockUnlock(gClientLock);
        }
    }
}

// ---- the bring-up ----
bool attachIrqs() {
    int rc;
    for (unsigned tries = 0;; ++tries) {
        rc = sm7675_remote_attach(kRemote, ipccDoorbell);
        if (rc != kIOReturnNotReady || tries == 240) break;
        IOSleep(250);
    }
    if (rc) fail(StDoorbells, rc);
    return rc == 0;
}

// The firmware comes from the device's own firmware partition ("modem:", read in place) or, with adsp-fw=DIR, from DIR/adsp.mbn
// and DIR/adsp_dtb.mbn on a mounted volume. Implicit selection uses only the stock partition.
// The UFS driver may not be up when this thread starts: a few tries with growing pauses (about two
// minutes in all), not a poll. source gets the source that opened ("modem:" or a directory).
int openPair(Image &dtb, Image &adsp, const char *source, char *path, size_t size) {
    const bool modem = !strcmp(source, "modem:");
    snprintf(path, size, modem ? "%sadsp_dtb" : "%s/adsp_dtb.mbn", source);
    int rc = openImage(dtb, path);
    if (rc) { release(dtb); return rc; }
    snprintf(path, size, modem ? "%sadsp" : "%s/adsp.mbn", source);
    if ((rc = openImage(adsp, path))) { release(adsp); release(dtb); }
    return rc;
}
int openFirmware(Image &dtb, Image &adsp, char *path, size_t size, const char *chosen) {
    static const uint32_t pauses[] = {1, 2, 4, 8, 16, 30, 30, 30};
    const char *sources[] = {"modem:"};
    const unsigned count = 1;
    if (chosen) sources[0] = chosen;
    int rc = 0;
    for (unsigned i = 0;; ++i) {
        int first = 0;
        for (unsigned s = 0; s < count; ++s) {
            rc = openPair(dtb, adsp, sources[s], path, size);
            if (!rc) return 0;
            if (!s) first = rc;
            if (rc != ENOENT && rc != ENXIO && rc != EIO) return rc;  // found but unusable: no other source is tried
        }
        rc = first;  // the reason the first source failed is the one worth reporting
        if (i == sizeof pauses / sizeof pauses[0]) return rc;
        IOSleep(pauses[i] * 1000);
    }
}

bool stopImage(Image &image) {
    if (!image.secureMetadata && !image.pasActive) return true;
    const ScmResult result = scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), image.pas, 0, 0);
    if (!scmOk(result)) {
        printf("UKE ADSP: quarantine pas=%u owned resources and power scm=%lld/%llu\n", image.pas,
            (long long)result.x0, (unsigned long long)result.x1);
        return false;
    }
    image.secureMetadata = false;
    image.pasActive = false;
    printf("UKE ADSP: shutdown confirmed pas=%u before metadata release\n", image.pas);
    return true;
}

void unwindStart() {
    // Match the target OEM qcom_mdt_free_metadata contract: shutdown before
    // free on error, including INIT_IMAGE failure. A refused shutdown retains
    // the metadata and dependent power references until a clean boot.
    if (!stopImage(gAdspImage) || !stopImage(gDtbImage)) return;
    release(gAdspImage);
    release(gDtbImage);
    if (gProxyHeld && vote(0, 0)) return;
    if (gLoadStateOn && !sm7675_remote_qmp("{class: image, res: load_state, name: adsp, val: off}"))
        gLoadStateOn = false;
}

bool start() {
    char dir[80] = "";
    const bool chosen = PE_parse_boot_argn("adsp-fw", dir, sizeof dir) && dir[0];
    char path[112];
    Image &dtb = gDtbImage, &adsp = gAdspImage;
    int rc = openFirmware(dtb, adsp, path, sizeof path, chosen ? dir : nullptr);
    if (rc) {note("fw: ", path); fail(StFirmware, rc); return false;}
    native_adsp_stats[7] = (int32_t)(adsp.size >> 10);
    stage(StFirmware);
    if (native_adsp_stats[3] > 0) {fail(StFirmware, -114); release(dtb); release(adsp); return false;}  // running already: left alone

    const Image *const images[] = {&dtb, &adsp};
    for (const auto *image : images) {
        const uint64_t base = image == &dtb ? kDtbBase : kAdspBase;
        const uint64_t size = image == &dtb ? kDtbSize : kAdspSize;
        for (unsigned i = 0; i < image->mdt.count; ++i) {
            uint64_t offset = 0;
            if (!image->mdt.segment[i].load || !qcom_mdt_place(&image->mdt, i, base, size, &offset)) continue;
            fail(StFirmware, -7); release(dtb); release(adsp); return false;
        }
    }

    // Our SMP2P item, before the DSP starts (it looks for it at boot).
    if ((rc = allocItem(kItemOut, QCOM_SMP2P_ITEM_BYTES))) {fail(StSmp2p, rc); release(dtb); release(adsp); return false;}
    qcom_smp2p_init_out(&gP2p, reinterpret_cast<volatile uint32_t *>(item(kItemOut, nullptr)), 0, kRemotePid);
    gStopEntry = qcom_smp2p_add_entry(&gP2p, "master-kernel");
    stage(StSmp2p);
    if (!attachIrqs()) {release(dtb); release(adsp); return false;}
    kickSmp2p();
    stage(StDoorbells);

    rc = sm7675_remote_qmp("{class: image, res: load_state, name: adsp, val: on}");
    native_adsp_stats[5] = (native_adsp_stats[5] & ~0xff) | (rc ? 0 : 4);
    if (rc) {fail(StLoadState, rc); release(dtb); release(adsp); return false;}
    gLoadStateOn = true;
    stage(StLoadState);

    // A copy of the command database, held only while the levels are looked up (a static copy would sit in the kernel's __DATA).
    struct CmdDbCopy {uint8_t *bytes = static_cast<uint8_t *>(IOMallocData(kCmdDbSize)); ~CmdDbCopy() {if (bytes) IOFreeData(bytes, kCmdDbSize);}} copy;
    uint8_t *const db = copy.bytes;
    uint32_t lcx = 0, lmx = 0;
    if (!db || !readCmdDb(db) || qcom_cmd_db_find(db, kCmdDbSize, "lcx.lvl", &gLcx) || qcom_cmd_db_find(db, kCmdDbSize, "lmx.lvl", &gLmx) ||
        qcom_cmd_db_find(db, kCmdDbSize, "xo.lvl", &gXo) ||
        qcom_rpmh_level_index(&gLcx, 384u, &lcx) || qcom_rpmh_level_index(&gLmx, 384u, &lmx)) {
        fail(StRails, -2); release(dtb); release(adsp); return false;
    }
    if ((rc = vote(lcx, lmx))) {fail(StRails, rc); release(dtb); release(adsp); return false;}
    stage(StRails);

    // The DSP's devicetree image first, as its own authenticated image, then the DSP image.
    if ((rc = authenticateHeader(dtb, kPasDtb, kDtbBase)) || (rc = loadSegments(dtb, kDtbBase, kDtbSize)) ||
        (rc = scmOk(scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPasDtb, 0, 0)) ? 0 : -3)) {
        fail(StDtb, rc); release(dtb); release(adsp); return false;
    }
    stage(StDtb);
    if ((rc = authenticateHeader(adsp, kPasAdsp, kAdspBase)) || (rc = loadSegments(adsp, kAdspBase, kAdspSize))) {
        fail(StLoaded, rc); release(dtb); release(adsp); return false;
    }
    stage(StLoaded);
    const uint64_t began = mach_absolute_time();
    if (!scmOk(scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPasAdsp, 0, 0))) {
        fail(StStarted, -3); release(dtb); release(adsp); return false;
    }
    stage(StStarted);
    const bool ready = waitUntil(5000, [] {return (gSlave & (kReady | kFatal)) != 0;});
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    native_adsp_stats[8] = (int32_t)(ns / 1000000);
    if (!ready || (gSlave & kFatal)) {
        readCrashReason();
        fail(StReady, ready ? -5 : -60);
        return false;
    }
    // The OEM releases metadata after authentication and start completion.
    // Runtime firmware ownership is independent of these transient buffers.
    dtb.secureMetadata = adsp.secureMetadata = false;
    release(dtb);
    release(adsp);
    stage(StReady);
    printf("UKE ADSP: PAS ready %d ms SMP2P=%x\n", native_adsp_stats[8], gSlave);
    return true;
}

bool startGlink() {
    int rc = allocItem(kItemGlinkDesc, 32);
    if (!rc) rc = allocItem(kItemGlinkTx, kGlinkFifo);
    uint32_t size = 0;
    volatile uint8_t *desc = item(kItemGlinkDesc, nullptr), *txf = item(kItemGlinkTx, &size);
    if (rc || !desc || !txf) {fail(StGlink, rc ? rc : -2); return false;}
    volatile uint32_t *d = reinterpret_cast<volatile uint32_t *>(desc);
    // Descriptor words: our FIFO's tail and head, then the DSP's FIFO's tail and head.
    const QcomGlinkFifo tx = {&d[1], &d[0], txf, size}, rx = {&d[3], &d[2], nullptr, 0};
    qcom_glink_init(&gGlink, tx, rx, kickGlink, nullptr);
    __atomic_add_fetch(&gGeneration, 1u, __ATOMIC_RELEASE);
    gGlinkStarted = true;
    applyClients();
    serviceGlink();
    return true;
}

void adspThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "adsp");
    unsigned mode = 0;
    PE_parse_boot_argn("adsp", &mode, sizeof mode);
    if (!mapWindow(gSmemW, kSmem, kSmemSize) || !mapWindow(gMutex, kMutex, 0x1000) ||
        !mapWindow(gAoss, kAoss, 0x1000) || !mapWindow(gRsc, kRscDrv, 0x10000)) {
        fail(StMapped, -12);
        return;
    }
    gSmem = {gSmemW.va, (uint32_t)kSmemSize};
    stage(StMapped);
    survey();
    stage(StSurveyed);
    if (mode < 2) return;
    if (!start() || !startGlink()) {
        unwindStart();
        return;
    }
    // From here on the thread only answers doorbells and client sends; with nothing pending it sleeps without a deadline, except while
    // a send waits for an intent or FIFO room (then it looks again every second).
    for (;;) {
        bool pending = false;
        IOLockLock(gClientLock);
        for (unsigned i = 0; i < __atomic_load_n(&gClientCount, __ATOMIC_ACQUIRE); ++i) pending |= gClients[i].count != 0;
        IOLockUnlock(gClientLock);
        service(waitEvents(pending ? 1000 : 3600u * 1000u) | (pending ? kEvSend : 0));
    }
}
}  // namespace

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                                const ShengAdspClient *client, void *context) {
    if (!gClientLock || !name || !client || !intent_size || !intent_count || intent_count > QCOM_GLINK_MAX_LOCAL_INTENTS || intent_size > 0x10000 ||
        strnlen(name, QCOM_GLINK_NAME) >= QCOM_GLINK_NAME) return nullptr;
    uint8_t *storage = static_cast<uint8_t *>(IOMallocData((vm_size_t)intent_size * intent_count));
    Message *queue = static_cast<Message *>(IOMallocData(sizeof(Message) * kQueue));
    IOLockLock(gClientLock);
    ShengAdspChannel *c = gClientCount < kClients && storage && queue ? &gClients[gClientCount] : nullptr;
    if (c) {
        bzero(c, sizeof *c);
        strlcpy(c->name, name, sizeof c->name);
        c->intent_size = intent_size; c->intent_count = intent_count; c->initiator = initiator;
        c->client = *client; c->context = context; c->storage = storage; c->queue = queue;
        __atomic_store_n(&gClientCount, gClientCount + 1, __ATOMIC_RELEASE);
    }
    IOLockUnlock(gClientLock);
    if (!c) {
        if (storage) IOFreeData(storage, (vm_size_t)intent_size * intent_count);
        if (queue) IOFreeData(queue, sizeof(Message) * kQueue);
        return nullptr;
    }
    wake(kEvSend);
    return c;
}

extern "C" int sheng_adsp_send(ShengAdspChannel *c, const void *data, uint32_t length) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(c), base = reinterpret_cast<uintptr_t>(gClients);
    if (!gClientLock || !c || !data || !length || at < base || (at - base) % sizeof(*c) ||
        (at - base) / sizeof(*c) >= __atomic_load_n(&gClientCount, __ATOMIC_ACQUIRE)) return -2;
    if (length > SHENG_ADSP_MAX_MESSAGE) return -1;
    IOLockLock(gClientLock);
    if (!c->up || __atomic_load_n(&gTransportFailed, __ATOMIC_ACQUIRE)) {IOLockUnlock(gClientLock); return -2;}
    if (c->count == kQueue) {IOLockUnlock(gClientLock); return -1;}
    Message &m = c->queue[(c->head + c->count) % kQueue];
    memcpy(m.data, data, length);
    m.length = length;
    c->count++;
    IOLockUnlock(gClientLock);
    wake(kEvSend);
    return 0;
}

extern "C" uint32_t sheng_adsp_generation(void) {return __atomic_load_n(&gGeneration, __ATOMIC_ACQUIRE);}

extern "C" void sheng_adsp_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("adsp", &mode, sizeof mode) || !mode || mode > 2 || native_adsp_stats[0]) return;
    gClientLock = IOLockAlloc();
    if (gClientLock && mode == 2) sheng_qrtr_start();
    thread_t thread = nullptr;
    if (!gClientLock || kernel_thread_start(adspThread, nullptr, &thread) != KERN_SUCCESS) {fail(StMapped, -7); return;}
    thread_deallocate(thread);
}
