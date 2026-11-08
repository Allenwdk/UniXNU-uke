// SPDX-License-Identifier: BSD-2-Clause-Patent
// Uke WCN6750 WPSS startup, derived from UniXNU-sheng AdspProbe.cpp at
// 460179565a018c741b93e0b1efa07c55d4de497a. Protocol cores stay unchanged
// in Sm7675Platform/remoteproc. Board addresses come from uke-live.dts.
// Mode 1 surveys only; mode 2 authenticates/starts WPSS and discovers WLFW.
// It does not publish a network interface before an ath11k data path exists.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>
#include "UkeWpssMemory.h"
#include "BootResetCore.h"
#include "ArmSmmu4K.h"
#include "ArmSmmu4KContext.h"
#include "SmmuBypass.h"
extern "C" {
#include "QcomGlink.h"
#include "QcomMdt.h"
#include "QcomQmp.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"
#include "UkeWpss.h"
int sheng_fw_open(const char *path, void **handle, uint64_t *size);
void uke_wifi_qrtr_start(void);
void uke_wifi_qrtr_tick(void);
void nw_ahb_qrtr_input(const void *data, size_t length);
void nw_ahb_qrtr_up(void);
void nw_ahb_qrtr_down(void);
int uke_wifi_ath11k_start(void);
int lx_pump(void);
void uke_wifi_msi_stats(unsigned *counts, unsigned *vectors);
void uke_wifi_station_step(void);
void uke_wifi_scan_selftest(void);
struct net_device *nw_cfg80211_primary(void);
/* Only the fields the progress line reports; the layout follows nw_abi.h. */
struct nw_wifi_stats { uint32_t stage, scans, frames, beacons, bss_seen; int last_err; };
extern struct nw_wifi_stats nw_wifi_stats;
void *gAhbGlChannel;
uint32_t uke_wifi_qrtr_wait_ms(void);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);
}

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
// [0] stage (see Stage; negative: the step that failed), [1] last error, [2] IPCC interrupts, [3] the DSP's slave-kernel bits (-1 no
// item), [4] items (bit 0 SMP2P in 443, 1 out 429, 2 GLINK 478, 3 479, 4 480, 8 our partition, 9 global partition), [5] QMP state | AOSS
// link bits << 8, [6] RSC DRV id, [7] adsp.mbn KiB, [8] ms from the start call to "ready", [9] GLINK messages in, [10] out, [11] channels
// up (bit per client) | version agreed << 14 | link broken << 15, [12] channel opens nobody serves, [13] last SCM x0, [14] its x1,
// [15] votes: lcx index | lmx index << 8 | RSC done interrupts << 16
extern "C" {volatile int32_t native_wifi_wpss_stats[16]; char native_wifi_wpss_text[96];}

namespace {
enum Stage {
    StMapped = 1, StSurveyed = 2, StFirmware = 3, StSmp2p = 4, StDoorbells = 5, StLoadState = 6, StRails = 7, StDtb = 8, StLoaded = 9,
    StStarted = 10, StReady = 11, StGlink = 12, StChannel = 13, StSleepClock = 15,
};
constexpr uint64_t kSmem = 0x81d00000, kSmemSize = 0x200000, kMutex = 0x1f40000 + 3 * 0x1000, kIpcc = 0x406000, kAoss = 0xc300000;
constexpr uint64_t kRscDrv = 0x17a20000, kRscTcs = 0xd00, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;
constexpr uint64_t kWpssBase = 0x9bb00000, kWpssSize = 0x1900000;
constexpr uint32_t kPasWpss = 6, kRemotePid = 13, kItemIn = 617, kItemOut = 616, kItemCrash = 626;
constexpr uint32_t kItemGlinkDesc = 478, kItemGlinkTx = 479, kItemGlinkRx = 480, kGlinkFifo = 0x4000;
constexpr uint32_t kSpiIpcc = 229, kSpiRsc = 5, kClientWpss = 24;
constexpr uint32_t kFatal = 1u << 0, kReady = 1u << 1, kHandover = 1u << 2;
constexpr uint32_t kEvSmp2p = 1, kEvGlink = 2, kEvAop = 4, kEvRsc = 8, kEvSend = 16;
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
void wr(const Window &w, uint64_t at, uint32_t value) {*reinterpret_cast<volatile uint32_t *>(w.va + at) = value; __asm__ volatile("dsb sy" ::: "memory");}
// sheng's PMK8550 sleep-clock setup, with uke DT inputs verified separately:
// spmi_bus EE 0, SID 0, GPIO base 0xb800, gpio3/func1, output, no bias, VIN0.
struct SleepClockSpmi {
    Window core{}, cnfg{}, obs{}, chn{};
    ~SleepClockSpmi() {unmap(chn); unmap(obs); unmap(cnfg); unmap(core);}
};

int sleepClockRead(volatile uint32_t *observer, const br_arbiter &arb, int apid, uint8_t *values) {
    static const uint8_t regs[] = {0x04, 0x05, 0x40, 0x41, 0x42, 0x44, 0x46};
    for (unsigned i = 0; i < sizeof regs; ++i) {
        if (br_read8(observer, &arb, apid, regs[i], &values[i])) return -EIO;
    }
    return 0;
}

bool sleepClockConfigured(const uint8_t *values) {
    return (values[2] & 3u) == 1u && (values[3] & 7u) == 0u &&
           (values[4] & 7u) == 5u && values[5] == 2u && (values[6] & 0x80u);
}

int prepareSleepClock() {
    constexpr uint32_t kSleepClockPpid = 0x0ba;
    SleepClockSpmi p;
    if (!mapWindow(p.core, 0xc400000, 0x3000) ||
        !mapWindow(p.cnfg, 0xc42d000, 0x4000)) return -ENOMEM;
    br_arbiter arb{reinterpret_cast<volatile uint32_t *>(p.core.va),
                   reinterpret_cast<volatile uint32_t *>(p.cnfg.va), 0};
    const uint32_t version = arb.core[0];
    const uint32_t apids = arb.core[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
    printf("UKE WIFI: sleepclk arb=%08x apids=%u ppid=%03x\n", version, apids, kSleepClockPpid);
    if (version < BR_VERSION_V7_MIN || version >= BR_VERSION_V8_MIN || !apids || apids > 1024)
        return -ENOTSUP;
    int apid = -1, owner = -1;
    for (uint32_t i = 0; i < apids; ++i) {
        const uint32_t map = arb.core[(BR_APID_MAP + 4 * i) / 4];
        if (!map || ((map >> 8) & 0xfff) != kSleepClockPpid) continue;
        const int candidateOwner = arb.cnfg[(BR_CNFG_OWNER + 4 * i) / 4] & 7u;
        if (apid < 0 || candidateOwner == arb.ee) {
            apid = i;
            owner = candidateOwner;
        }
    }
    if (apid < 0) return -ENOENT;
    if (!mapWindow(p.obs, 0xc440000, (BR_OBS_APID_STRIDE * apids + 0xfffu) & ~0xfffULL) ||
        !mapWindow(p.chn, 0xc500000, uint64_t(BR_CHNL_APID_STRIDE) * apids)) return -ENOMEM;
    auto *observer = reinterpret_cast<volatile uint32_t *>(p.obs.va);
    auto *channels = reinterpret_cast<volatile uint32_t *>(p.chn.va);
    uint8_t before[7] = {};
    if (sleepClockRead(observer, arb, apid, before)) return -EIO;
    printf("UKE WIFI: sleepclk before apid=%d owner=%d type=%02x subtype=%02x mode=%02x vin=%02x pull=%02x source=%02x enable=%02x\n",
           apid, owner, before[0], before[1], before[2], before[3], before[4], before[5], before[6]);
    if (before[0] != 0x10 || before[1] < 0x10 || before[1] > 0x13) return -ENOTSUP;
    if (sleepClockConfigured(before)) {
        printf("UKE WIFI: sleepclk gpio3 already configured\n");
        return 0;
    }
    if (owner != arb.ee) return -EPERM;
    // Same order as sheng pmicPrepare: pull, VIN, function, direction, enable.
    static const uint8_t config[][2] = {{0x42, 5}, {0x41, 0}, {0x44, 2}, {0x40, 1}, {0x46, 0x80}};
    for (const auto &reg : config) {
        if (br_write8(channels, apid, reg[0], reg[1])) return -EIO;
    }
    uint8_t after[7] = {};
    if (sleepClockRead(observer, arb, apid, after)) return -EIO;
    printf("UKE WIFI: sleepclk after mode=%02x vin=%02x pull=%02x source=%02x enable=%02x\n",
           after[2], after[3], after[4], after[5], after[6]);
    return sleepClockConfigured(after) ? 0 : -EIO;
}

void fail(int step, int error) {native_wifi_wpss_stats[1] = error; native_wifi_wpss_stats[0] = -step; printf("UKE WIFI: fail stage=%d error=%d scm=%d/%d\n", step, error, native_wifi_wpss_stats[13], native_wifi_wpss_stats[14]);}
void stage(int value) {native_wifi_wpss_stats[0] = value; printf("UKE WIFI: stage=%d slave=%x items=%x\n", value, (unsigned)native_wifi_wpss_stats[3], (unsigned)native_wifi_wpss_stats[4]);}
void note(const char *prefix, const char *text) {
    size_t at = 0;
    for (; prefix[at] && at < sizeof native_wifi_wpss_text - 1; ++at) native_wifi_wpss_text[at] = prefix[at];
    for (size_t i = 0; text[i] && at < sizeof native_wifi_wpss_text - 1; ++i) native_wifi_wpss_text[at++] = text[i];
    native_wifi_wpss_text[at] = 0;
    printf("UKE WIFI: %s\n", native_wifi_wpss_text);
}

// apps_smmu@15000000 faults unmatched streams (the UFS port read sCR0.USFCFG
// back set). The WLAN SID has no stream-match entry, so firmware writes into
// the fixed 12 MB MSA region would fault. Reuse the last context bank as a
// stage-1-off bypass, the arrangement UFS validated on this board, and route
// the masked WLAN stream through it before WPSS starts.
constexpr uint64_t kAppsSmmu = 0x15000000, kAppsSmmuSize = 0x100000;
constexpr uint32_t kWlanStream = 0x1400; /* DT mask 1 is mapped as two exact entries */
constexpr uint32_t kScr0 = 0x00, kIdr0 = 0x20, kIdr1 = 0x24, kIdr2 = 0x28, kGfsr = 0x48;
constexpr uint32_t kSmrBase = 0x800, kS2crBase = 0xc00;

struct SmmuGeometry {unsigned count, banks, pages; bool usable;};
SmmuGeometry smmuGeometry(const Window &w) {
    const uint32_t idr0 = rd(w, kIdr0), idr1 = rd(w, kIdr1), idr2 = rd(w, kIdr2);
    SmmuGeometry g{static_cast<unsigned>(idr0 & 0xff), static_cast<unsigned>(idr1 & 0xff),
                   1U << (((idr1 >> 28) & 7) + 1), false};
    g.usable = (idr0 & (1U << 30)) && (idr2 & (1U << 12)) && g.count && g.banks >= 2 &&
               g.count <= 128 && g.banks <= 128 && g.pages <= 128 &&
               (g.pages + g.banks) * 4096 <= kAppsSmmuSize;
    return g;
}
bool smmuStreamMatches(uint32_t smr, uint32_t stream) {
    if (!(smr & 0x80000000U)) return false;
    return ((smr ^ stream) & 0xffff & ~((smr >> 16) & 0x7fff)) == 0;
}
void smmuReport(const char *when) {
    printf("UKE WIFI: SMMU %s begin\n", when);
    Window w{};
    if (!mapWindow(w, kAppsSmmu, kAppsSmmuSize)) {printf("UKE WIFI: SMMU %s map failed\n", when); return;}
    printf("UKE WIFI: SMMU %s mapped\n", when);
    const SmmuGeometry g = smmuGeometry(w);
    printf("UKE WIFI: SMMU %s idr0=%08x idr1=%08x idr2=%08x usable=%u count=%u banks=%u pages=%u\n",
           when, rd(w, kIdr0), rd(w, kIdr1), rd(w, kIdr2), g.usable ? 1 : 0, g.count, g.banks, g.pages);
    printf("UKE WIFI: SMMU %s global scr0=%08x sacr=%08x gfsr=%08x\n", when, rd(w, kScr0), rd(w, 0x10), rd(w, kGfsr));
    unsigned match = 0xffff, smrs = 0, banks = 0, clean = 0; uint32_t matchSmr = 0, matchS2cr = 0;
    for (unsigned i = 0; i < g.count && g.count <= 128; ++i) {
        const uint32_t smr = rd(w, kSmrBase + 4 * i);
        const uint32_t s2cr = rd(w, kS2crBase + 4 * i);
        if (!(smr & 0x80000000U)) {if (!s2cr) clean++; continue;}
        if (smmuStreamMatches(smr, kWlanStream) && match == 0xffff) {match = i; matchSmr = smr; matchS2cr = s2cr;}
        smrs++; printf("UKE WIFI: SMMU %s smr[%u]=%08x s2cr=%08x\n", when, i, smr, s2cr);
    }
    unsigned zero_s2cr = 0, s2cr70 = 0, other = 0;
    for (unsigned i = 0; i < g.count && g.count <= 128; ++i) {
        const uint32_t smr = rd(w, kSmrBase + 4 * i), s2cr = rd(w, kS2crBase + 4 * i);
        if ((smr & 0x80000000U) || !s2cr) {if (!s2cr) zero_s2cr++; continue;}
        if (s2cr == 0x46) {if (s2cr70 < 12) printf("UKE WIFI: SMMU %s s2cr70[%u] smr=%08x\n", when, i, smr); s2cr70++;}
        else if (other < 12) {other++; printf("UKE WIFI: SMMU %s s2cr_other[%u]=%08x\n", when, i, s2cr);}
    }
    printf("UKE WIFI: SMMU %s clean_slots=%u zero_s2cr=%u s2cr70=%u other=%u\n", when, clean, zero_s2cr, s2cr70, other);
    for (unsigned b = 0; b < g.banks && g.banks <= 128 && banks < 24; ++b) {
        const uint32_t sctlr = rd(w, (g.pages + b) * 4096);
        const uint32_t cbar = rd(w, 0x1000 + 4 * b), cba2r = rd(w, 0x1800 + 4 * b);
        if (!sctlr && !cbar && !cba2r) continue;
        banks++; printf("UKE WIFI: SMMU %s bank[%u] sctlr=%08x cbar=%08x cba2r=%08x\n", when, b, sctlr, cbar, cba2r);
    }
    printf("UKE WIFI: SMMU %s scr0=%08x gfsr=%08x sid=%x match=%x smr=%08x s2cr=%08x smrs=%u banks=%u\n",
           when, rd(w, kScr0), rd(w, kGfsr), kWlanStream, match, matchSmr, matchS2cr, smrs, banks);
    unmap(w);
    printf("UKE WIFI: SMMU %s done\n", when);
}
// Route the WLAN stream to the bypass bank that touch/UFS already share.
// A slot counts as free only when both SMR and S2CR read zero: an invalid
// SMR with a stale S2CR is not a proven free group. The 2 s pause lets the
// userdata writer flush the chosen slot/bank before any write happens.
const char *smmuRouteWlan(void) {
    Window w{};
    if (!mapWindow(w, kAppsSmmu, kAppsSmmuSize)) return "apps SMMU map failed";
    const SmmuGeometry g = smmuGeometry(w);
    if (!g.usable) {unmap(w); return "unsupported apps SMMU geometry";}
    unsigned bank = g.banks, slot = g.count;
    for (unsigned i = 0; i < g.count; ++i) {
        const uint32_t smr = rd(w, kSmrBase + 4 * i), s2cr = rd(w, kS2crBase + 4 * i);
        if ((smr & 0x80000000U) && smmuStreamMatches(smr, kWlanStream)) {unmap(w); return "WLAN stream already mapped";}
        if ((smr & 0x80000000U) && ((s2cr >> 16) & 3) == 0) bank = s2cr & 0xff;
    }
    if (bank < g.banks) for (unsigned i = 0; i < g.count; ++i) {
        const uint32_t smr = rd(w, kSmrBase + 4 * i);
        if (!(smr & 0x80000000U) && slot == g.count) slot = i;
    }
    if (bank >= g.banks) {unmap(w); return "no bypass bank referenced by a live stream";}
    const uint32_t sctlr = rd(w, (g.pages + bank) * 4096), cbar = rd(w, 0x1000 + 4 * bank), cba2r = rd(w, 0x1800 + 4 * bank);
    printf("UKE WIFI: SMMU route bank=%u sctlr=%08x cbar=%08x cba2r=%08x slot=%u\n", bank, sctlr, cbar, cba2r, slot);
    if (sctlr || cbar != 0x10000 || cba2r != 1) {unmap(w); return "live bypass bank readback mismatch";}
    if (slot == g.count) {unmap(w); return "no stream-match group";}
    unmap(w);
    IOSleep(2100);
    Window x{};
    if (!mapWindow(x, kAppsSmmu, kAppsSmmuSize)) return "apps SMMU remap failed";
    for (unsigned sid = kWlanStream; sid <= kWlanStream + 1; ++sid) {
        unsigned free = g.count;
        for (unsigned i = 0; i < g.count; ++i) {
            const uint32_t smr = rd(x, kSmrBase + 4 * i);
            if (smr & 0x80000000U) {if ((smr & 0xffff) == sid) {unmap(x); return "WLAN stream already mapped";}}
            else if (free == g.count) free = i;
        }
        if (free == g.count) {unmap(x); return "no stream-match group";}
        const uint32_t oldS2cr = rd(x, kS2crBase + 4 * free);
        const uint32_t smr = 0x80000000U | sid;
        printf("UKE WIFI: SMMU step1 SMR-only sid=%x slot=%u old_s2cr=%08x\n", sid, free, oldS2cr);
        IOSleep(2100);
        wr(x, kSmrBase + 4 * free, smr);
        printf("UKE WIFI: SMMU step1 readback smr=%08x s2cr=%08x\n", rd(x, kSmrBase + 4 * free), rd(x, kS2crBase + 4 * free));
        IOSleep(2100);
        const uint32_t oldS2cr2 = rd(x, kS2crBase + 4 * free);
        printf("UKE WIFI: SMMU step2 sid=%x S2CR->bank%u old=%08x\n", sid, bank, oldS2cr2);
        wr(x, kS2crBase + 4 * free, bank);
        const uint32_t rs = rd(x, kSmrBase + 4 * free), rc = rd(x, kS2crBase + 4 * free);
        printf("UKE WIFI: SMMU step2 readback smr=%08x s2cr=%08x\n", rs, rc);
        if (rs != smr || rc != bank) {unmap(x); return "stream mapping readback mismatch";}
    }
    unmap(x);
    smmuReport("route");
    return nullptr;
}

// The QMI window alone does not prove CPU MMIO access. These offsets follow
// the original ath11k static AHB windows in ahb.c, hw.c, hal.h and pcic.h.
constexpr uint32_t kWlanBarBytes = 0x200000;
constexpr uint64_t kUkeWlanBar = 0x61e00000;
constexpr uint32_t kWlanWindow = 0x80000;
constexpr uint32_t kReo1Misc = kWlanWindow + (0x00a38000 + 0x1fc - 0x00a00000);
constexpr uint32_t kCe0DstControl = 2 * kWlanWindow + (0x01b81000 + 0xb0 - 0x01b80000);
static_assert(kReo1Misc == 0xb81fc && kCe0DstControl == 0x1010b0);
static_assert(kReo1Misc < kWlanBarBytes && kCe0DstControl < kWlanBarBytes);

Window gSmemW, gMutex, gIpcc, gAoss, gRsc;
QcomSmem gSmem;
QcomSmemPartition gPart;
QcomSmp2p gP2p;
int gStopEntry = -1;
QcomQmp gQmp;
QcomGlink gGlink;
bool gGlinkStarted;
QcomRscLayout gLayout;
uint32_t gRscIrqBits;
volatile uint32_t gPending, gRscDone;

// ---- clients ----
struct Message {uint32_t length; uint8_t data[UKE_WPSS_MAX_MESSAGE];};
}  // namespace
extern "C" {volatile int32_t native_wifi_wpss_tx[8];}
struct UkeWpssChannel {
    char name[QCOM_GLINK_NAME];
    uint32_t intent_size, intent_count;
    int initiator;
    UkeWpssClient client;
    void *context;
    uint8_t *storage;
    QcomGlinkChannel *glink;
    Message *queue;  // kQueue slots
    uint32_t head, count;
};
namespace {
UkeWpssChannel gClients[kClients];
uint32_t gClientCount;
IOLock *gClientLock;

void wake(uint32_t bits) {
    __atomic_fetch_or(&gPending, bits, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}

// ---- interrupts: acknowledge, record, wake ----
void ipccIrq(void *) {
    uint32_t bits = 0;
    for (unsigned n = 0; n < 32; ++n) {
        const uint32_t id = rd(gIpcc, QCOM_IPCC_RECV_ID);
        if (id == QCOM_IPCC_NONE) break;
        wr(gIpcc, QCOM_IPCC_RECV_CLEAR, id);
        if (id == qcom_ipcc_id(kClientWpss, QCOM_IPCC_SIGNAL_SMP2P)) bits |= kEvSmp2p;
        else if (id == qcom_ipcc_id(kClientWpss, QCOM_IPCC_SIGNAL_GLINK_QMP)) bits |= kEvGlink;
        else if (id == qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP)) bits |= kEvAop;
    }
    native_wifi_wpss_stats[2] = native_wifi_wpss_stats[2] + 1;
    if (bits) wake(bits);
}
void rscIrq(void *) {
    const uint32_t status = rd(gRsc, kRscTcs + gLayout.irq_status) & gRscIrqBits;
    if (!status) return;
    wr(gRsc, kRscTcs + gLayout.irq_clear, status);
    __atomic_fetch_or(&gRscDone, status, __ATOMIC_RELEASE);
    native_wifi_wpss_stats[15] = native_wifi_wpss_stats[15] + (1 << 16);
    wake(kEvRsc);
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

void ring(uint32_t client, uint32_t signal) {wr(gIpcc, QCOM_IPCC_SEND, qcom_ipcc_id(client, signal));}
void kickAop(void *) {ring(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP);}
void kickGlink(void *) {ring(kClientWpss, QCOM_IPCC_SIGNAL_GLINK_QMP);}
void kickSmp2p() {ring(kClientWpss, QCOM_IPCC_SIGNAL_SMP2P);}

// ---- SMEM helpers (allocation under the TCSR hardware mutex) ----
bool lockSmem() {
    // The mutex is held only for the few writes of one allocation by whichever processor has it; it is tried a bounded number of times.
    for (unsigned i = 0; i < 1000; ++i) {
        wr(gMutex, 0, 1);
        if (rd(gMutex, 0) == 1) return true;
        IODelay(10);
    }
    return false;
}
void unlockSmem() {wr(gMutex, 0, 0);}
int allocItem(uint16_t item, uint32_t size) {
    if (!lockSmem()) return -16;
    const int rc = qcom_smem_alloc(&gSmem, &gPart, item, size);
    unlockSmem();
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

// ---- the survey (uke-wifi=1 and the first step of 2): reads only ----
void survey() {
    int32_t bits = 0;
    if (!qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_APPS, kRemotePid, &gPart)) {
        bits |= 1 << 8;
        const uint16_t ids[5] = {kItemIn, kItemOut, kItemGlinkDesc, kItemGlinkTx, kItemGlinkRx};
        for (unsigned i = 0; i < 5; ++i) if (item(ids[i], nullptr)) bits |= 1 << i;
    }
    QcomSmemPartition global;
    if (!qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global)) bits |= 1 << 9;
    native_wifi_wpss_stats[4] = bits;
    native_wifi_wpss_stats[3] = -1;
    if (volatile uint8_t *in = item(kItemIn, nullptr)) {
        QcomSmp2p view = {};
        view.in = reinterpret_cast<volatile uint32_t *>(in);
        view.local_pid = 0; view.remote_pid = kRemotePid;
        const int index = qcom_smp2p_check_in(view.in, 0, kRemotePid) ? -1 : qcom_smp2p_find_in(&view, "slave-kernel");
        if (index >= 0) native_wifi_wpss_stats[3] = (int32_t)qcom_smp2p_in_value(&view, index);
    }
    const uint32_t up = 0xffff;
    native_wifi_wpss_stats[5] = (rd(gAoss, 0) == 0x4d41494cu ? 1 : 0) << 12 | (rd(gAoss, 0xc) == up) << 8 | (rd(gAoss, 0x24) == up) << 9 |
                           (rd(gAoss, 0x14) == up) << 10 | (rd(gAoss, 0x2c) == up) << 11;
    native_wifi_wpss_stats[6] = (int32_t)rd(gRsc, 0);
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
QcomRpmhResource gCx, gMx, gXo;
int gTcs = -1;

// DT channel0 reserves active TCS0..2. This port has no other AP-RSC
// consumer; GMU uses its separate GPU RSC. Refuse a live TCS1 rather than
// overwrite it. CX/MX are shared with the running GPU/UFS. Until a shared
// vote owner exists, this diagnostic holds their maximum cmd-db levels
// through handover instead of clearing the AP's shared resource votes.
int vote(uint32_t cx, uint32_t mx, uint32_t xo) {
    if (gTcs < 0) {
        gTcs = 1;
        const uint32_t control = kRscTcs + gLayout.tcs_stride * gTcs + gLayout.control;
        if (rd(gRsc, control) & (1u << 24)) return -16;
        gRscIrqBits = 1u << gTcs;
        wr(gRsc, kRscTcs, rd(gRsc, kRscTcs) | gRscIrqBits);
    }
    const uint32_t addresses[3] = {gCx.address, gMx.address, gXo.address}, values[3] = {cx, mx, xo};
    QcomRpmhWrite writes[20];
    const unsigned n = qcom_rpmh_active_request(&gLayout, (unsigned)gTcs, addresses, values, 3, writes, 20);
    if (!n) return -22;
    __atomic_store_n(&gRscDone, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < n; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
    uint64_t deadline;
    clock_interval_to_deadline(100, kMillisecondScale, &deadline);
    while (!(__atomic_load_n(&gRscDone, __ATOMIC_ACQUIRE) & gRscIrqBits) && mach_absolute_time() < deadline) service(waitEvents(5));
    const bool done = (__atomic_load_n(&gRscDone, __ATOMIC_ACQUIRE) & gRscIrqBits) ||
                      (rd(gRsc, kRscTcs + gLayout.irq_status) & gRscIrqBits);
    native_wifi_wpss_stats[15] = (native_wifi_wpss_stats[15] & ~0xffff) | (int32_t)(cx | mx << 8 | (done ? 0 : 1u << 15));
    if (!done) return -60;
    const unsigned m = qcom_rpmh_finish(&gLayout, (unsigned)gTcs, writes, 20);
    for (unsigned i = 0; i < m; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
    return 0;
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
    native_wifi_wpss_stats[13] = (int32_t)result.x0;
    native_wifi_wpss_stats[14] = (int32_t)result.x1;
    return result;
}
bool scmOk(const ScmResult &r) {return r.x0 == 0 && r.x1 == 0;}

void cleanToPoc(const void *start, size_t length) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- a firmware image: parsed from its file, metadata handed over, segments written into the carveout ----
struct Image {  // plain data, zero at start: the two live in static storage (a few KiB of header table each, kept off the stack)
    void *file;
    uint64_t size;
    QcomMdtImage mdt;
    IOBufferMemoryDescriptor *meta;
};
Image gWpssImage;
int fileRead(void *context, uint64_t offset, void *to, size_t length) {return sheng_fw_read(context, offset, to, length);}
void release(Image &image) {
    if (image.file) sheng_fw_close(image.file);
    if (image.meta) image.meta->release();
    image.file = nullptr; image.meta = nullptr;
}
int openImage(Image &image, const char *path) {
    int rc = sheng_fw_open(path, &image.file, &image.size);
    if (rc) {image.file = nullptr; return rc;}
    rc = qcom_mdt_parse(&image.mdt, image.size, fileRead, image.file);
    return rc ? 1000 - rc : 0;
}
// init_image with the metadata in physically contiguous memory, then the memory setup of a relocatable image.
int authenticateHeader(Image &image, uint32_t pas, uint64_t base) {
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes,
                                                                 0xfffffffffffff000ull);
    if (!image.meta || image.meta->prepare() != kIOReturnSuccess) return -12;
    uint8_t *va = static_cast<uint8_t *>(image.meta->getBytesNoCopy());
    if (qcom_mdt_metadata(&image.mdt, fileRead, image.file, va, bytes)) return -5;
    cleanToPoc(va, bytes);
    const uint64_t phys = image.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
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
        volatile uint8_t *to = region.va + at;
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (sheng_fw_read(image.file, s.file_offset + done, bounce, n)) {rc = -5; break;}
            uke_wpss_memory_copy(to + done, bounce, n);
            done += n;
        }
        if (!rc && s.memory_size > s.file_size)
            uke_wpss_memory_zero(to + s.file_size, (size_t)(s.memory_size - s.file_size));
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (bounce) IOFreeData(bounce, kChunk);
    unmap(region);
    return rc;
}

// ---- events ----
uint32_t gSlave;
bool gHandedOver;
void serviceSmp2p() {
    if (!gP2p.in) {
        if (volatile uint8_t *in = item(kItemIn, nullptr)) gP2p.in = reinterpret_cast<volatile uint32_t *>(in);
        else return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gP2p, "slave-kernel", &value, &changed)) kickSmp2p();
    gSlave = value;
    native_wifi_wpss_stats[3] = (int32_t)value;
    if ((changed & value & kFatal)) readCrashReason();
    if ((value & kHandover) && !gHandedOver) {
        gHandedOver = true;
        note("handover: ", "WPSS ready; shared CX/MX/XO votes retained");
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
    qcom_glink_poll(&gGlink);
    pumpSends();
    uint32_t up = 0;
    for (unsigned i = 0; i < gClientCount; ++i) if (gClients[i].glink && gClients[i].glink->up) up |= 1u << i;
    native_wifi_wpss_stats[9] = (int32_t)gGlink.rx_messages;
    native_wifi_wpss_stats[10] = (int32_t)gGlink.tx_messages;
    native_wifi_wpss_stats[11] = (int32_t)(up | (uint32_t)gGlink.version_agreed << 14 | (uint32_t)gGlink.broken << 15);
    native_wifi_wpss_stats[12] = (int32_t)gGlink.unknown_opens;
    if (gGlink.unknown_opens && strncmp(native_wifi_wpss_text, "crash: ", 7) &&
        (strncmp(native_wifi_wpss_text, "open: ", 6) ||
         strncmp(native_wifi_wpss_text + 6, gGlink.unknown_names, sizeof(native_wifi_wpss_text) - 7)))
        note("open: ", gGlink.unknown_names);
    if (gGlink.version_agreed && native_wifi_wpss_stats[0] >= StReady && native_wifi_wpss_stats[0] < StGlink) stage(StGlink);
    if (up && native_wifi_wpss_stats[0] == StGlink) stage(StChannel);
}
void service(uint32_t bits) {
    if (bits & kEvAop) qcom_qmp_step(&gQmp);
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
    UkeWpssChannel *c = static_cast<UkeWpssChannel *>(context);
    if (c->client.state) c->client.state(c->context, up);
}
void clientReceive(void *context, QcomGlinkChannel *, const uint8_t *data, uint32_t length) {
    UkeWpssChannel *c = static_cast<UkeWpssChannel *>(context);
    if (c->client.receive) c->client.receive(c->context, data, length);
}
void applyClients() {
    if (!gGlinkStarted) return;
    const QcomGlinkClient glue = {clientState, clientReceive};
    IOLockLock(gClientLock);
    for (unsigned i = 0; i < gClientCount; ++i) {
        UkeWpssChannel &c = gClients[i];
        if (!c.glink) c.glink = qcom_glink_channel(&gGlink, c.name, c.intent_size, c.intent_count, c.storage, &glue, &c, c.initiator);
    }
    IOLockUnlock(gClientLock);
}
// The audio channel's send state, for the log: [0] sent, [1] deferred (no intent or FIFO room), [2] refused otherwise, [3] queued now,
// [4] intent asked (0/1), [5] valid remote intents, [6] the largest remote intent, [7] the last deferred length
void noteChannel(const UkeWpssChannel &c, int rc, uint32_t length) {
    if (strcmp(c.name, "IPCRTR")) return;
    if (rc == QcomGlinkOk) native_wifi_wpss_tx[0] = native_wifi_wpss_tx[0] + 1;
    else if (rc == QcomGlinkAgain) {native_wifi_wpss_tx[1] = native_wifi_wpss_tx[1] + 1; native_wifi_wpss_tx[7] = (int32_t)length;}
    else native_wifi_wpss_tx[2] = native_wifi_wpss_tx[2] + 1;
    native_wifi_wpss_tx[3] = (int32_t)c.count;
    native_wifi_wpss_tx[4] = c.glink->intent_asked;
    int32_t valid = 0, largest = 0;
    for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k)
        if (c.glink->remote[k].valid) {++valid; if ((int32_t)c.glink->remote[k].size > largest) largest = (int32_t)c.glink->remote[k].size;}
    native_wifi_wpss_tx[5] = valid;
    native_wifi_wpss_tx[6] = largest;
}
void pumpSends() {
    for (unsigned i = 0; i < gClientCount; ++i) {
        UkeWpssChannel &c = gClients[i];
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
bool gIrq;
bool attachIrqs() {
    if (!armcpu_spi_attach) return false;
    int rc = 0;
    for (unsigned tries = 0;; ++tries) {  // the GIC driver may not have started yet (as ButtonProbe found)
        rc = armcpu_spi_attach(32 + kSpiIpcc, false, ipccIrq, nullptr, nullptr, nullptr);
        if (rc != kIOReturnNotReady || tries == 240) break;
        IOSleep(250);
    }
    if (rc) {fail(StDoorbells, rc); return false;}
    rc = armcpu_spi_attach(32 + kSpiRsc, false, rscIrq, nullptr, nullptr, nullptr);
    if (rc) {fail(StDoorbells, rc); return false;}
    const uint32_t enable[3] = {qcom_ipcc_id(kClientWpss, QCOM_IPCC_SIGNAL_SMP2P),
                                qcom_ipcc_id(kClientWpss, QCOM_IPCC_SIGNAL_GLINK_QMP),
                                qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP)};
    for (uint32_t id : enable) wr(gIpcc, QCOM_IPCC_RECV_ENABLE, id);
    gIrq = true;
    return true;
}

// Read signed split firmware from the stock FAT partition through the
// upstream reader. Optional mounted-file override is explicit; no fallback
// is attempted for a present but malformed image.
int openFirmware(Image &image) {
    char path[112] = "modem:qca6750/wpss";
    PE_parse_boot_argn("uke-wifi-fw", path, sizeof path);
    static const uint32_t pauses[] = {1, 2, 4, 8, 16, 30, 30, 30};
    for (unsigned i = 0;; ++i) {
        const int rc = openImage(image, path);
        if (!rc) return 0;
        release(image);
        if ((rc != ENOENT && rc != ENXIO && rc != EIO) || i == sizeof pauses / sizeof pauses[0]) {
            note("fw: ", path); return rc;
        }
        IOSleep(pauses[i] * 1000);
    }
}

bool start() {
    Image &image = gWpssImage;
    if (native_wifi_wpss_stats[3] > 0) {fail(StFirmware, -114); return false;}
    int rc = prepareSleepClock();
    if (rc) {fail(StSleepClock, rc); return false;}
    stage(StSleepClock);
    rc = openFirmware(image);
    if (rc) {fail(StFirmware, rc); return false;}
    native_wifi_wpss_stats[7] = (int32_t)(image.size >> 10);
    // Validate every segment before metadata authentication or any carveout write.
    for (unsigned i = 0; i < image.mdt.count; ++i) {
        uint64_t offset = 0;
        if (image.mdt.segment[i].load && qcom_mdt_place(&image.mdt, i, kWpssBase, kWpssSize, &offset)) {
            fail(StFirmware, -7); release(image); return false;
        }
    }
    stage(StFirmware);
    if ((rc = allocItem(kItemOut, QCOM_SMP2P_ITEM_BYTES))) {fail(StSmp2p, rc); release(image); return false;}
    qcom_smp2p_init_out(&gP2p, reinterpret_cast<volatile uint32_t *>(item(kItemOut, nullptr)), 0, kRemotePid);
    gStopEntry = qcom_smp2p_add_entry(&gP2p, "master-kernel");
    // ICNSS entries share this same outbound item, prepared before WPSS starts.
    qcom_smp2p_add_entry(&gP2p, "wlan");
    qcom_smp2p_add_entry(&gP2p, "wlan_soc_wake");
    qcom_smp2p_add_entry(&gP2p, "wlan_ep_power_save");
    stage(StSmp2p);
    if (!attachIrqs()) {release(image); return false;}
    kickSmp2p();
    stage(StDoorbells);
    qcom_qmp_init(&gQmp, reinterpret_cast<volatile uint32_t *>(gAoss.va), 0x400, kickAop, nullptr);
    if (!waitUntil(4000, [] {return qcom_qmp_step(&gQmp) == QcomQmpOpen || gQmp.state < 0;}) || gQmp.state != QcomQmpOpen) {
        fail(StLoadState, gQmp.state); release(image); return false;
    }
    qcom_qmp_send(&gQmp, "{class: image, res: load_state, name: wpss, val: on}");
    if (!waitUntil(1000, [] {return qcom_qmp_step(&gQmp) == QcomQmpOpen;})) {
        qcom_qmp_abandon(&gQmp); fail(StLoadState, -60); release(image); return false;
    }
    stage(StLoadState);
    // Temporary command database storage must not consume the fileset's
    // limited native DATA/BSS window beside retained Apple kexts.
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    uint32_t cx = 0, mx = 0;
    const bool resources = db && readCmdDb(db) && !qcom_cmd_db_find(db, kCmdDbSize, "cx.lvl", &gCx) &&
        !qcom_cmd_db_find(db, kCmdDbSize, "mx.lvl", &gMx) && !qcom_cmd_db_find(db, kCmdDbSize, "xo.lvl", &gXo) &&
        !qcom_rpmh_level_index(&gCx, 0xffffffffu, &cx) && !qcom_rpmh_level_index(&gMx, 0xffffffffu, &mx);
    if (db) IOFreeData(db, kCmdDbSize);
    if (!resources) {
        fail(StRails, -2); release(image); return false;
    }
    if ((rc = vote(cx, mx, 3))) {fail(StRails, rc); release(image); return false;}
    stage(StRails);
    if ((rc = authenticateHeader(image, kPasWpss, kWpssBase)) || (rc = loadSegments(image, kWpssBase, kWpssSize))) {
        fail(StLoaded, rc); scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPasWpss, 0, 0);
        release(image); return false;
    }
    stage(StLoaded);
    const uint64_t began = mach_absolute_time();
    if (!scmOk(scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPasWpss, 0, 0))) {
        fail(StStarted, -3); scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPasWpss, 0, 0);
        release(image); return false;
    }
    stage(StStarted);
    const bool ready = waitUntil(5000, [] {return (gSlave & (kReady | kFatal)) != 0;});
    uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    native_wifi_wpss_stats[8] = (int32_t)(ns / 1000000);
    release(image);
    if (!ready || (gSlave & kFatal)) {
        readCrashReason(); fail(StReady, ready ? -5 : -60);
        scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPasWpss, 0, 0);
        return false;
    }
    stage(StReady); return true;
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
    gGlinkStarted = true;
    applyClients();
    serviceGlink();
    return true;
}

// Build the WLAN MSA stage-1 tables and context image in real kernel memory,
// using the encoders the GPU path already links. Nothing is written to the
// SMMU: activation needs a free bank and a quiescent moment, both still open.
void wlanMsaPlan(void) {
    Window smmu{};
    if (!mapWindow(smmu, kAppsSmmu, kAppsSmmuSize)) {printf("UKE WIFI: MSA plan smmu map failed\n"); return;}
    const uint32_t id0 = rd(smmu, kIdr0), id2 = rd(smmu, kIdr2);
    unmap(smmu);
    const uint64_t kPoolBytes = 16 * 4096;
    IOBufferMemoryDescriptor *pool = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, kPoolBytes, 0xfffffff000ULL);
    if (!pool || pool->prepare() != kIOReturnSuccess) {
        printf("UKE WIFI: MSA plan pool failed\n");
        if (pool) pool->release();
        return;
    }
    const uint64_t pa = pool->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    uint64_t *tables = static_cast<uint64_t *>(pool->getBytesNoCopy());
    ArmSmmu4KConfig cfg{};
    cfg.input_bits = 36; cfg.output_bits = 36; cfg.table_physical = pa; cfg.leaf_budget = 16 * 512;
    ArmSmmu4KRange range{};
    range.iova = 0xa6400000ULL; range.physical = 0xa6400000ULL; range.bytes = 0x00c00000ULL;
    range.permissions = ArmSmmu4KRead | ArmSmmu4KWrite; range.writeback = 1; range.shareability = 3;
    ArmSmmu4KResult result{};
    ArmSmmu4KStatus st = ArmSmmu4KBuild(&cfg, &range, 1, tables, 16, &result);
    printf("UKE WIFI: MSA plan build=%d pa=%llx root=%llx pages=%zu leaves=%zu level=%u\n", (int)st,
           (unsigned long long)pa, (unsigned long long)result.root_physical, result.table_pages,
           result.leaf_pages, result.start_level);
    if (st == ArmSmmu4KSuccess) {
        ArmSmmu4KContextConfig cc{};
        cc.lower = &cfg; cc.lower_tables = &result; cc.id0 = id0; cc.id2 = id2;
        cc.asid = 1; cc.coherent_walk = 1;
        ArmSmmu4KContextImage image{};
        st = ArmSmmu4KEncodeContext(&cc, &image);
        printf("UKE WIFI: MSA context=%d cbar=%08x cba2r=%08x tcr=%08x tcr2=%08x mair0=%08x mair1=%08x sctlr=%08x ttbr0=%llx\n",
               (int)st, image.cbar, image.cba2r, image.tcr, image.tcr2, image.mair0, image.mair1,
               image.sctlr, (unsigned long long)image.ttbr0);
    }
    pool->complete();
    pool->release();
}

extern "C" uint64_t uke_wpss_now_ms(void);
// Intent accounting while the board driver uploads: a stalled upload shows
// all remote intents busy, or none valid with intent_asked still set.
extern "C" int native_ufs_extra_fault(uint32_t *out) __attribute__((weak_import));
void debugStats() {
    static uint64_t last;
    const uint64_t now = uke_wpss_now_ms();
    if (now - last < 5000) return;
    last = now;
    unsigned valid = 0, busy = 0, up = 0, asked = 0;
    uint32_t queued = 0;
    for (unsigned i = 0; i < gClientCount; ++i) {
        UkeWpssChannel &c = gClients[i];
        queued += c.count;
        if (!c.glink) continue;
        up += c.glink->up;
        asked = c.glink->intent_asked;
        for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k)
            if (c.glink->remote[k].valid) {++valid; if (c.glink->remote[k].busy) ++busy;}
    }
    printf("UKE WIFI AHB: glink tx=%u rx=%u q=%u clients=%u up=%u rvalid=%u rbusy=%u asked=%u\n",
           gGlink.tx_messages, gGlink.rx_messages, queued, gClientCount, up, valid, busy, asked);
    // Vectors the endpoint actually raised. The firmware writes 0x320 + ce_id
    // (the AHB hybrid hif ops install no compacting get_ce_msi_idx), while the
    // host registers handlers at compacted CE indices. For CE0..CE3 those two
    // agree, which is exactly the set the HTC/HTT/WMI bring-up needs; a vector
    // whose count stays zero is a ring the driver is still waiting on.
    unsigned counts[32] = {}, mask = 0;
    uke_wifi_msi_stats(counts, &mask);
    printf("UKE WIFI AHB: msi mask=%08x ce0-8=%u %u %u %u %u %u %u %u %u\n", mask,
           counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7], counts[8]);
    // The default interface appears only once cfg80211 finishes registering the
    // radio, which happens on the QMI worker after this loop is running; report
    // it here rather than at start time.
    printf("UKE WIFI AHB: station primary %s stage %u scans %u frames %u bss %u\n",
           nw_cfg80211_primary() ? "present" : "absent", nw_wifi_stats.stage,
           nw_wifi_stats.scans, nw_wifi_stats.frames, nw_wifi_stats.bss_seen);
    static uint32_t lastFault[10];
    uint32_t fault[10] = {};
    if (&native_ufs_extra_fault != nullptr && native_ufs_extra_fault(fault) &&
        memcmp(fault, lastFault, sizeof fault) != 0) {
        memcpy(lastFault, fault, sizeof fault);
        printf("UKE WIFI AHB: smmu gfsr=%08x gsynr0=%08x cbfsr=%08x far=%08x%08x fsynr0=%08x sctlr=%08x tcr=%08x ttbr0=%08x cbar=%08x\n",
               fault[0], fault[1], fault[2], fault[4], fault[3], fault[5], fault[6], fault[7], fault[8], fault[9]);
    }
}

extern "C" volatile uint64_t nw_wifi_where[4];
// Independent witness: reports where the LX dispatcher last was and for how
// long, so a wedged work item is visible even when the worker cannot print.
void ahbWatchdog(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "uke-wifi-wd");
    for (unsigned i = 0; i < 10; ++i) {
        IOSleep(2000);
        const uint64_t now = uke_wpss_now_ms();
        const uint64_t since = nw_wifi_where[3];
        printf("UKE WIFI WD: t=%llu stage=%d kind=%llu fn=%llx depth=%llu stuck=%llums tx=%u rx=%u\n",
               now, native_wifi_wpss_stats[0], nw_wifi_where[0], nw_wifi_where[1], nw_wifi_where[2],
               since ? now - since : 0, gGlink.tx_messages, gGlink.rx_messages);
    }
}

extern "C" void lx_irq_polling(int enabled);
// The endpoint's MSI address is a page in the DMA window (see dma_map_resource):
// each interrupt is a 32-bit data write into it. The vector is decoded and the
// matching LX handler raised by the platform poll callback (WifiAth11k.cpp,
// pollGl -> msiPoll), which is the one the dispatcher runs at the top of every
// lx_pump() -- including the pumps nested inside ath11k's synchronous waits. The
// board must not also read the page here: whichever reader runs first consumes
// the single latched word.

void ahbReceive(void *, const uint8_t *data, uint32_t length) {nw_ahb_qrtr_input(data, length);}
void ahbState(void *, int up) {if (up) nw_ahb_qrtr_up(); else nw_ahb_qrtr_down();}

void wpssThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "uke-wifi");
    unsigned mode = 3;
    PE_parse_boot_argn("uke-wifi", &mode, sizeof mode);
    if (!mapWindow(gSmemW, kSmem, kSmemSize) || !mapWindow(gMutex, kMutex, 0x1000) || !mapWindow(gIpcc, kIpcc, 0x1000) ||
        !mapWindow(gAoss, kAoss, 0x1000) || !mapWindow(gRsc, kRscDrv, 0x10000)) {
        fail(StMapped, -12);
        return;
    }
    gSmem = {gSmemW.va, (uint32_t)kSmemSize};
    gLayout = qcom_rsc_layout(rd(gRsc, 0));
    stage(StMapped);
    smmuReport("boot");
    wlanMsaPlan();
    survey();
    stage(StSurveyed);
    if (mode < 2) return;
    if (mode == 3) {
        if (!start() || !startGlink()) return;
        static const UkeWpssClient ahbClient = {ahbState, ahbReceive};
        gAhbGlChannel = uke_wpss_channel("IPCRTR", 0x4400, 10, 0, &ahbClient, nullptr);
        if (!gAhbGlChannel) {fail(StChannel, -7); return;}
        lx_irq_polling(0);
        if (uke_wifi_ath11k_start()) {fail(StChannel, -8); return;}
        thread_t watchdog = nullptr;
        if (kernel_thread_start(ahbWatchdog, nullptr, &watchdog) == KERN_SUCCESS) thread_deallocate(watchdog);
        stage(14);
        // uke_wifi_station_step() runs the module's own lifecycle, which owns
        // lx_pump (control poll, dispatcher, station run); the board does not
        // pump a second time per iteration. service(waitEvents(10)) must run
        // every round: it consumes kEvSend (the control worker's wake-up) and
        // drives applyClients()/serviceGlink(). V4.9 briefly skipped service()
        // whenever nw_wifi_step() reported busy; that regressed the control TX
        // path (zero controlSend calls, all INFO sessions timing out) and let
        // WMI dev_close/dev_open flood the HTC credit=1 channel. Reverted.
        for (;;) {uke_wifi_station_step(); uke_wifi_scan_selftest(); debugStats(); service(waitEvents(10));}
    }
    const char *smmuError = smmuRouteWlan();
    if (smmuError) printf("UKE WIFI: SMMU route failed: %s\n", smmuError);
    if (!start() || !startGlink()) return;
    // From here on the thread only answers doorbells and client sends; with nothing pending it sleeps without a deadline, except while
    // a send waits for an intent or FIFO room (then it looks again every second).
    for (;;) {
        bool pending = false;
        for (unsigned i = 0; i < gClientCount; ++i) pending |= gClients[i].count != 0;
        uke_wifi_qrtr_tick();
        service(waitEvents(pending ? 1000 : uke_wifi_qrtr_wait_ms()) | (pending ? kEvSend : 0));
    }
}
}  // namespace

// The ath11k QMI client waits synchronously for replies. Its waits run the
// LX dispatcher, whose board poll calls this to move GLINK frames, so a reply
// can be delivered while a request is outstanding.
extern "C" void uke_wpss_glink_poll(void) {
    if (!gGlinkStarted) return;
    applyClients();
    serviceGlink();
}

extern "C" void uke_wifi_msa_report(void) {
    Window w{};
    if (!mapWindow(w, 0xa6400000ULL, 0x00c00000ULL)) {printf("UKE WIFI: MSA map failed\n"); return;}
    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(w.va);
    printf("UKE WIFI: MSA[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
           p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    uint32_t hits = 0, first = 0;
    for (uint32_t off = 0; off < 0x40000; off += 4) {
        if (p[off / 4] == 0x464c457fU) {if (!hits) first = off; hits++;}
    }
    printf("UKE WIFI: MSA elf_hits=%u first=%x\n", hits, first);
    unmap(w);
}

extern "C" int uke_wifi_msa_write(uint64_t offset, const void *data, uint32_t length) {
    if (offset + length > 0x00c00000ULL) return -1;
    Window w{};
    if (!mapWindow(w, 0xa6400000ULL, 0x00c00000ULL)) return -2;
    volatile uint8_t *dst = w.va + offset;
    const volatile uint8_t *src = static_cast<const volatile uint8_t *>(data);
    for (uint32_t i = 0; i < length; ++i) dst[i] = src[i];
    __asm__ volatile("dsb sy" ::: "memory");
    unmap(w);
    return 0;
}

extern "C" void uke_wpss_crash_report(void) {
    uint32_t size = 0;
    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(item(kItemCrash, &size));
    if (!p) {printf("UKE WIFI: WPSS crash item unavailable\n"); return;}
    printf("UKE WIFI: WPSS crash item bytes=%u words=%08x %08x %08x %08x %08x %08x\n", size,
           p[0], p[1], p[2], p[3], p[4], p[5]);
}

extern "C" void uke_wifi_ahb_wake(void) {
    wake(kEvSend);
}

extern "C" void uke_wifi_smmu_report(const char *when) {
    smmuReport(when);
}

extern "C" void uke_wifi_probe_bar(uint64_t base, uint32_t size) {
    if (base != kUkeWlanBar || size != kWlanBarBytes) {
        printf("UKE WIFI: MMIO declined BAR=%llx size=%x\n",
               static_cast<unsigned long long>(base), size);
        return;
    }
    Window window{};
    if (!mapWindow(window, base, size)) {
        printf("UKE WIFI: MMIO mapping failed\n");
        return;
    }
    printf("UKE WIFI: MMIO read REO1_MISC offset=%x\n", kReo1Misc);
    const uint32_t reoMisc = rd(window, kReo1Misc);
    printf("UKE WIFI: MMIO REO1_MISC=%08x\n", reoMisc);
    printf("UKE WIFI: MMIO read CE0_DST_CTRL offset=%x\n", kCe0DstControl);
    const uint32_t ceControl = rd(window, kCe0DstControl);
    printf("UKE WIFI: MMIO CE0_DST_CTRL=%08x\n", ceControl);
    unmap(window);
}

extern "C" uint64_t uke_wpss_now_ms(void) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}

extern "C" UkeWpssChannel *uke_wpss_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                                const UkeWpssClient *client, void *context) {
    if (!gClientLock || !name || !client || !intent_size || !intent_count || intent_count > QCOM_GLINK_MAX_LOCAL_INTENTS) return nullptr;
    uint8_t *storage = static_cast<uint8_t *>(IOMallocData((vm_size_t)intent_size * intent_count));
    Message *queue = static_cast<Message *>(IOMallocData(sizeof(Message) * kQueue));
    IOLockLock(gClientLock);
    UkeWpssChannel *c = gClientCount < kClients && storage && queue ? &gClients[gClientCount] : nullptr;
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

extern "C" int uke_wpss_send(UkeWpssChannel *c, const void *data, uint32_t length) {
    if (!c || !data || !length) return -2;
    if (length > UKE_WPSS_MAX_MESSAGE) return -1;
    IOLockLock(gClientLock);
    if (c->count == kQueue) {IOLockUnlock(gClientLock); return -1;}
    Message &m = c->queue[(c->head + c->count) % kQueue];
    memcpy(m.data, data, length);
    m.length = length;
    c->count++;
    IOLockUnlock(gClientLock);
    wake(kEvSend);
    return 0;
}

extern "C" void uke_wpss_probe(void) {
    unsigned mode = 3;
    PE_parse_boot_argn("uke-wifi", &mode, sizeof mode);
    if (!mode || mode > 3 || native_wifi_wpss_stats[0]) return;
    gClientLock = IOLockAlloc();
    if (gClientLock && mode == 2) uke_wifi_qrtr_start();  // the router link registers before the DSP can open it
    thread_t thread = nullptr;
    if (!gClientLock || kernel_thread_start(wpssThread, nullptr, &thread) != KERN_SUCCESS) {fail(StMapped, -7); return;}
    thread_deallocate(thread);
}
