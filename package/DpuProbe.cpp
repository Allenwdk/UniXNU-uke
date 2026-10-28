// SPDX-License-Identifier: BSD-2-Clause-Patent
// DPU bring-up probe and controlled-pipe test for the uke board (cliffs SM7675).
//
// A3 (read-only, boot argument uke-dpu=<n>): one snapshot of the display
// processing unit the firmware left running, so the DPU work copies the live
// GOP configuration instead of guessing. The register map is
// docs/dts-reference/mdss-cliffs-map.md (DTS-derived); the block anchors are the
// same DPU 9.x anchors sheng's DisplayTimingProbe uses (SM8550), verified against
// the cliffs DTB by scripts/check-dpu-probe-host.py.
//   1  snapshot: MDSS/MDP top, VIG0/VIG1, DMA0..4, CTL0..3, INTF1/2, PP0..3,
//      LM0/1, two frame-counter samples 1 s apart, GOP geometry.
//   2  as 1, plus display clock branch state and the display SMMU bank state.
//
// C2 (bounded, boot argument uke-dpu-test=<n>): the first deliberate writes.
// Every touched register is saved first and restored when the hold ends, so the
// firmware's configuration comes back without a reboot. Nothing runs unless the
// boot argument selects a level.
//   1  write each live VIG pipe's own source address back and flush CTL 0:
//      measures the latch and proves the write path; the picture cannot change.
//   2  solid fill on the live VIG pipes (upstream dpu_hw_sspp_setup_solidfill:
//      SRC_FORMAT bit 22 + SRC_CONSTANT_COLOR), hold, restore.
//   3  move the live VIG pipes to a 3200x2136 BGRA gradient buffer mapped into
//      the display SMMU's stage-1 window (native_ufs_dpu_map, the ShengUfs
//      mechanism); hold, restore.
//   4  same picture through the free tail of the low-DRAM splash region (below
//      4 GiB, reachable while the display bank translates off), for boards where
//      the stage-1 window is not available yet.
//
// The probe never panics on a refused prerequisite: it logs "UKE DPU: ..." and
// leaves the display to the firmware. A write test that wedges the panel is
// recovered by a reboot (the firmware programs the DPU again); the restore path
// runs from the same thread after the hold.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>

extern "C" uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count) __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_dpu_state __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_dpu_smmu[24] __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_dpu_check[8] __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_dpu_atos[4] __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_dpu_fault_sid __attribute__((weak_import));
extern "C" uint32_t native_ufs_dpu_table(unsigned) __attribute__((weak_import));
extern "C" uint32_t native_ufs_dpu_l2(unsigned) __attribute__((weak_import));
extern "C" volatile uint32_t native_dpu_stage;

namespace {

// MDP block base. The DTS block offsets are relative to the mdss_mdp reg base
// 0xae00000; the DPU register block starts at sde-off 0x1000 (0xae01000), as on
// sheng, so every offset below is the DTS value minus 0x1000.
constexpr uint64_t kMdssBase = 0xae00000, kMdpBase = 0xae01000, kMdpBytes = 0x8f000;
constexpr uint64_t kDispccBase = 0xaf00000, kGccBase = 0x100000;
constexpr uint64_t kSmmuBase = 0x15000000, kSmmuBytes = 0x100000;

// SSPP VIG0/VIG1 and DMA0..4 (code offsets).
constexpr uint32_t kVig[2] = {0x4000, 0x6000};
constexpr uint32_t kDma[5] = {0x24000, 0x26000, 0x28000, 0x2a000, 0x2c000};
constexpr uint32_t kCtl[4] = {0x15000, 0x16000, 0x17000, 0x18000};
constexpr uint32_t kIntf[2] = {0x35000, 0x36000};   // INTF1 = DSI0, INTF2 = DSI1
constexpr uint32_t kPp[4] = {0x69000, 0x6a000, 0x6b000, 0x6c000};
constexpr uint32_t kLm[2] = {0x44000, 0x45000};

// SSPP registers (stock dpu_hw_sspp.c).
constexpr uint32_t kSrcSize = 0x00, kSrcXy = 0x08, kOutSize = 0x0c, kOutXy = 0x10;
constexpr uint32_t kSrc0 = 0x14, kSrc1 = 0x18, kSrc2 = 0x1c, kSrc3 = 0x20;
constexpr uint32_t kStride0 = 0x24, kStride1 = 0x28, kFormat = 0x30, kUnpack = 0x34;
constexpr uint32_t kOpMode = 0x38, kConstant = 0x3c, kUbwc = 0x44, kFetch = 0x48;
constexpr uint32_t kQos = 0x6c, kAddrStatus = 0x70;
// CTL registers (stock dpu_hw_ctl.c).
constexpr uint32_t kCtlLayer = 0x00, kCtlTop = 0x14, kCtlFlush = 0x18, kCtlStart = 0x1c;
constexpr uint32_t kCtlPrepare = 0xd0, kCtlFetch = 0xfc, kCtlIntfFlush = 0x110, kCtlIntfMaster = 0x134;
// INTF registers (stock dpu_hw_intf.c).
constexpr uint32_t kIntfEn = 0x000, kIntfConfig = 0x004, kIntfHsync = 0x008, kIntfVperiod = 0x00c;
constexpr uint32_t kIntfCount = 0x0a8, kIntfFrame = 0x0ac, kIntfLine = 0x0b0, kIntfStatus = 0x26c;
constexpr uint32_t kIntfTear = 0x284, kIntfAfr = 0x2b4;
// PP registers (stock dpu_hw_pingpong.c).
constexpr uint32_t kPpTear = 0x000, kPpSync = 0x004, kPpOutLine = 0x028, kPpLine = 0x02c, kPpAfr = 0x030;
// LM registers (stock dpu_hw_lm.c).
constexpr uint32_t kLmOp = 0x000, kLmOut = 0x004, kLmBlend0 = 0x020;

constexpr uint64_t kSplashEnd = 0xe6440000ULL;      // splash_region@0xe3940000 + 0x2b00000
constexpr uint32_t kPatternWidth = 3200, kPatternHeight = 2136;
constexpr uint32_t kPatternStride = kPatternWidth * 4;
constexpr uint32_t kSmallWidth = 2048, kSmallHeight = 1024;

uint32_t gProbeLevel = 0, gTestLevel = 0;

uint32_t rd(volatile uint32_t *base, uint32_t offset) {return base ? base[offset / 4] : 0xffffffffU;}
void wr(volatile uint32_t *base, uint32_t offset, uint32_t value) {
    if (base) {
        base[offset / 4] = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
}

void cacheCleanInvalidate(const void *start, size_t length) {
    uintptr_t address = reinterpret_cast<uintptr_t>(start) & ~uintptr_t(63);
    const uintptr_t end = reinterpret_cast<uintptr_t>(start) + length;
    for (; address < end; address += 64) __asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

struct Map {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
    explicit Map(uint64_t base, uint64_t bytes) {
        IODeviceMemory *memory = IODeviceMemory::withRange(base, bytes);
        if (memory) {
            map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
            memory->release();
        }
        if (map) va = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
    }
    ~Map() {if (map) map->release();}
    uint32_t get(uint32_t offset) const {return va ? va[offset / 4] : 0xffffffffU;}
    void set(uint32_t offset, uint32_t value) const {
        if (va) {
            va[offset / 4] = value;
            __asm__ volatile("dsb sy" ::: "memory");
        }
    }
};

// Decode (height-1)<<16 | (width-1). 0 when the field is empty.
void decodeSize(uint32_t value, uint32_t *width, uint32_t *height) {
    *width = value & 0xffffU;
    *height = value >> 16;
    if (!*width && !*height) *width = *height = 0;
}

bool sameGeometry(uint32_t size, uint32_t width, uint32_t height) {
    uint32_t w = 0, h = 0;
    decodeSize(size, &w, &h);
    return w == width && h == height;
}

void logGop() {
    PE_Video video = {};
    if (PE_current_console(&video)) {
        IOLog("UKE DPU: gop unavailable\n");
        return;
    }
    const uint64_t bytes = (uint64_t)video.v_rowBytes * video.v_height;
    IOLog("UKE DPU: gop base=%llx bytes=%llu %ux%u rb=%u depth=%u\n",
          (unsigned long long)video.v_baseAddr, (unsigned long long)bytes,
          (unsigned)video.v_width, (unsigned)video.v_height, (unsigned)video.v_rowBytes, (unsigned)video.v_depth);
}

void snapshotPipe(volatile uint32_t *mdp, uint32_t base, const char *name) {
    const uint32_t size = rd(mdp, base + kSrcSize), xy = rd(mdp, base + kSrcXy);
    const uint32_t out = rd(mdp, base + kOutSize), outxy = rd(mdp, base + kOutXy);
    const uint32_t a0 = rd(mdp, base + kSrc0), a1 = rd(mdp, base + kSrc1);
    const uint32_t a2 = rd(mdp, base + kSrc2), a3 = rd(mdp, base + kSrc3);
    const uint32_t s0 = rd(mdp, base + kStride0), s1 = rd(mdp, base + kStride1);
    IOLog("UKE DPU: %s size=%08x xy=%08x out=%08x outxy=%08x src=%08x/%08x/%08x/%08x stride=%08x/%08x fmt=%08x unpack=%08x op=%08x\n",
          name, size, xy, out, outxy, a0, a1, a2, a3, s0, s1, rd(mdp, base + kFormat), rd(mdp, base + kUnpack), rd(mdp, base + kOpMode));
}

void snapshotExtendedPipe(volatile uint32_t *mdp, uint32_t base, const char *name) {
    IOLog("UKE DPU: %s fetch=%08x ubwc=%08x qos=%08x const=%08x addrsw=%08x\n",
          name, rd(mdp, base + kFetch), rd(mdp, base + kUbwc), rd(mdp, base + kQos),
          rd(mdp, base + kConstant), rd(mdp, base + kAddrStatus));
}

void snapshotInterface(volatile uint32_t *mdp, uint32_t base, const char *name) {
    IOLog("UKE DPU: %s en=%08x cfg=%08x cnt=%08x frame=%u line=%u hsync=%08x vperiod=%08x status=%08x\n",
          name, rd(mdp, base + kIntfEn), rd(mdp, base + kIntfConfig), rd(mdp, base + kIntfCount),
          rd(mdp, base + kIntfFrame), rd(mdp, base + kIntfLine), rd(mdp, base + kIntfHsync),
          rd(mdp, base + kIntfVperiod), rd(mdp, base + kIntfStatus));
}

void snapshotClocks() {
    Map gcc(kGccBase, 0x1f4200), dispcc(kDispccBase, 0x20000);
    IOLog("UKE DPU: clk gcc_axi=%08x ahb=%08x mdp=%08x lut=%08x vsync=%08x mdp_rcg=%08x\n",
          gcc.get(0x2700c), dispcc.get(0x80a4), dispcc.get(0x800c), dispcc.get(0x8018),
          dispcc.get(0x8024), dispcc.get(0x80d8));
}

void snapshotSmmu() {
    Map smmu(kSmmuBase, kSmmuBytes);
    if (!smmu.va) {
        IOLog("UKE DPU: smmu nomap\n");
        return;
    }
    const uint32_t idr0 = smmu.get(0x20), idr1 = smmu.get(0x24);
    const unsigned groups = (idr0 & 0xff) > 128 ? 128 : (idr0 & 0xff);
    const unsigned banks = idr1 & 0xff;
    const unsigned pages = 1U << (((idr1 >> 28) & 7) + 1);
    IOLog("UKE DPU: smmu scr0=%08x idr0=%08x idr1=%08x id2=%08x groups=%u banks=%u\n",
          smmu.get(0x0), idr0, idr1, smmu.get(0x28), groups, banks);
    for (unsigned i = 0; i < groups; ++i) {
        const uint32_t smr = smmu.get(0x800 + 4 * i);
        if (!(smr & 0x80000000U)) continue;
        const uint32_t mask = (smr >> 16) & 0x7fff;
        for (unsigned k = 0; k < 2; ++k) {
            if ((smr ^ (0x1c00U + 2 * k)) & 0x7fff & ~mask) continue;
            const uint32_t s2cr = smmu.get(0xc00 + 4 * i), bank = s2cr & 0xff;
            uint32_t cbar = 0xffffffffU, cba2r = 0xffffffffU, sctlr = 0xffffffffU;
            uint32_t ttbr0 = 0xffffffffU, ttbr1 = 0xffffffffU, tcr = 0xffffffffU;
            if (bank < banks && (pages + bank + 1) * 4096 <= kSmmuBytes) {
                const uint32_t cb = static_cast<uint32_t>((pages + bank) * 4096);
                cbar = smmu.get(0x1000 + 4 * bank);
                cba2r = smmu.get(0x1800 + 4 * bank);
                sctlr = smmu.get(cb + 0x0);
                ttbr0 = smmu.get(cb + 0x20);
                ttbr1 = smmu.get(cb + 0x24);
                tcr = smmu.get(cb + 0x30);
            }
            IOLog("UKE DPU: smmu sid=%04x smr=%08x s2cr=%08x cbar=%08x cba2r=%08x sctlr=%08x ttbr0=%08x/%08x tcr=%08x\n",
                  (unsigned)(0x1c00 + 2 * k), smr, s2cr, cbar, cba2r, sctlr, ttbr0, ttbr1, tcr);
        }
    }
}

void snapshot() {
    native_dpu_stage = 1;
    Map mdss(kMdssBase, 0x1000), mdp(kMdpBase, kMdpBytes);
    if (!mdp.va) {
        IOLog("UKE DPU: probe fail stage=map\n");
        return;
    }
    IOLog("UKE DPU: probe=%u stage=1 mdss=%08x mdp=%08x irq_en=%08x irq=%08x\n",
          gProbeLevel, mdss.get(0x0), mdp.get(0x0), mdp.get(0x10), mdp.get(0x14));
    logGop();
    for (unsigned i = 0; i < 2; ++i) {
        char name[8] = {'v', 'i', 'g', static_cast<char>('0' + i), 0};
        snapshotPipe(mdp.va, kVig[i], name);
        if (gProbeLevel >= 2) snapshotExtendedPipe(mdp.va, kVig[i], name);
    }
    for (unsigned i = 0; i < 5; ++i) {
        char name[8] = {'d', 'm', 'a', static_cast<char>('0' + i), 0};
        snapshotPipe(mdp.va, kDma[i], name);
    }
    for (unsigned i = 0; i < 4; ++i) {
        char name[8] = {'c', 't', 'l', static_cast<char>('0' + i), 0};
        IOLog("UKE DPU: %s top=%08x flush=%08x start=%08x prep=%08x fetch=%08x iflush=%08x master=%08x layer0=%08x\n",
              name, rd(mdp.va, kCtl[i] + kCtlTop), rd(mdp.va, kCtl[i] + kCtlFlush), rd(mdp.va, kCtl[i] + kCtlStart),
              rd(mdp.va, kCtl[i] + kCtlPrepare), rd(mdp.va, kCtl[i] + kCtlFetch), rd(mdp.va, kCtl[i] + kCtlIntfFlush),
              rd(mdp.va, kCtl[i] + kCtlIntfMaster), rd(mdp.va, kCtl[i] + kCtlLayer));
    }
    for (unsigned i = 0; i < 2; ++i) {
        char name[8] = {'i', 'n', 't', 'f', static_cast<char>('1' + i), 0};
        snapshotInterface(mdp.va, kIntf[i], name);
        if (gProbeLevel >= 2)
            IOLog("UKE DPU: %s tear=%08x afr=%08x\n", name, rd(mdp.va, kIntf[i] + kIntfTear), rd(mdp.va, kIntf[i] + kIntfAfr));
    }
    for (unsigned i = 0; i < 4; ++i) {
        char name[8] = {'p', 'p', static_cast<char>('0' + i), 0};
        IOLog("UKE DPU: %s tear=%08x sync=%08x outline=%08x line=%08x afr=%08x\n",
              name, rd(mdp.va, kPp[i] + kPpTear), rd(mdp.va, kPp[i] + kPpSync),
              rd(mdp.va, kPp[i] + kPpOutLine), rd(mdp.va, kPp[i] + kPpLine), rd(mdp.va, kPp[i] + kPpAfr));
    }
    for (unsigned i = 0; i < 2; ++i) {
        char name[8] = {'l', 'm', static_cast<char>('0' + i), 0};
        IOLog("UKE DPU: %s op=%08x out=%08x blend0=%08x\n",
              name, rd(mdp.va, kLm[i] + kLmOp), rd(mdp.va, kLm[i] + kLmOut), rd(mdp.va, kLm[i] + kLmBlend0));
    }
    native_dpu_stage = 2;
    const uint32_t frame0[2] = {rd(mdp.va, kIntf[0] + kIntfFrame), rd(mdp.va, kIntf[1] + kIntfFrame)};
    const uint32_t line0[2] = {rd(mdp.va, kIntf[0] + kIntfLine), rd(mdp.va, kIntf[1] + kIntfLine)};
    const uint64_t began = mach_absolute_time();
    IOSleep(1000);
    uint64_t elapsed = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &elapsed);
    IOLog("UKE DPU: live frame=%u->%u/%u->%u line=%u->%u/%u->%u us=%u\n",
          frame0[0], rd(mdp.va, kIntf[0] + kIntfFrame), frame0[1], rd(mdp.va, kIntf[1] + kIntfFrame),
          line0[0], rd(mdp.va, kIntf[0] + kIntfLine), line0[1], rd(mdp.va, kIntf[1] + kIntfLine),
          static_cast<unsigned>(elapsed / 1000));
    // The display bank's window is the B2 prerequisite; show it in every snapshot so a
    // failed C2 address test can be read from the same log.
    IOLog("UKE DPU: window state=%08x smmu0=%08x idr1=%08x check=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x\n",
          &native_ufs_dpu_state ? native_ufs_dpu_state : 0U,
          &native_ufs_dpu_smmu ? native_ufs_dpu_smmu[0] : 0U,
          &native_ufs_dpu_smmu ? native_ufs_dpu_smmu[3] : 0U,
          &native_ufs_dpu_check ? native_ufs_dpu_check[0] : 0U,
          &native_ufs_dpu_check ? native_ufs_dpu_check[1] : 0U,
          &native_ufs_dpu_check ? native_ufs_dpu_check[2] : 0U,
          &native_ufs_dpu_check ? native_ufs_dpu_check[3] : 0U,
          &native_ufs_dpu_check ? native_ufs_dpu_check[4] : 0U,
          &native_ufs_dpu_check ? native_ufs_dpu_check[5] : 0U,
          &native_ufs_dpu_check ? native_ufs_dpu_check[6] : 0U,
          &native_ufs_dpu_check ? native_ufs_dpu_check[7] : 0U);
    IOLog("UKE DPU: window atos iova=%08x par=%08x/%08x spin=%u faultsid=%04x l1=%08x/%08x/%08x/%08x l2_80=%08x l2_81=%08x\n",
          &native_ufs_dpu_atos ? native_ufs_dpu_atos[0] : 0U,
          &native_ufs_dpu_atos ? native_ufs_dpu_atos[1] : 0U,
          &native_ufs_dpu_atos ? native_ufs_dpu_atos[2] : 0U,
          &native_ufs_dpu_atos ? native_ufs_dpu_atos[3] : 0U,
          &native_ufs_dpu_fault_sid ? native_ufs_dpu_fault_sid : 0U,
          &native_ufs_dpu_table ? native_ufs_dpu_table(0) : 0U,
          &native_ufs_dpu_table ? native_ufs_dpu_table(1) : 0U,
          &native_ufs_dpu_table ? native_ufs_dpu_table(2) : 0U,
          &native_ufs_dpu_table ? native_ufs_dpu_table(3) : 0U,
          &native_ufs_dpu_l2 ? native_ufs_dpu_l2(128) : 0U,
          &native_ufs_dpu_l2 ? native_ufs_dpu_l2(129) : 0U);
    if (gProbeLevel >= 2) snapshotClocks();
    snapshotSmmu();
    native_dpu_stage = 3;
    IOLog("UKE DPU: probe done stage=3\n");
}

// The live pipe set: a VIG pipe whose size is the panel (or half-panel) size,
// or whose source address is inside the splash region the firmware scans.
unsigned livePipes(volatile uint32_t *mdp, uint32_t *pipes) {
    PE_Video video = {};
    const bool videoOk = !PE_current_console(&video);
    unsigned count = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t size = rd(mdp, kVig[i] + kSrcSize);
        const uint32_t addr = rd(mdp, kVig[i] + kSrc0);
        const bool sizeMatch = videoOk && (sameGeometry(size, video.v_width, video.v_height) ||
                                           sameGeometry(size, video.v_width / 2, video.v_height));
        const bool addrMatch = addr >= 0xe3940000U && addr < 0xe6440000U;
        if (sizeMatch || addrMatch) pipes[count++] = i;
    }
    return count;
}

struct SavedPipe {
    uint32_t src0, size, xy, out, outxy, stride0, stride1, format, unpack, opmode, constant;
};
SavedPipe savePipe(volatile uint32_t *mdp, unsigned index) {
    const uint32_t b = kVig[index];
    SavedPipe s = {rd(mdp, b + kSrc0), rd(mdp, b + kSrcSize), rd(mdp, b + kSrcXy), rd(mdp, b + kOutSize),
                   rd(mdp, b + kOutXy), rd(mdp, b + kStride0), rd(mdp, b + kStride1), rd(mdp, b + kFormat),
                   rd(mdp, b + kUnpack), rd(mdp, b + kOpMode), rd(mdp, b + kConstant)};
    return s;
}
void restorePipe(volatile uint32_t *mdp, unsigned index, const SavedPipe &s) {
    const uint32_t b = kVig[index];
    wr(mdp, b + kSrc0, s.src0); wr(mdp, b + kSrcSize, s.size); wr(mdp, b + kSrcXy, s.xy);
    wr(mdp, b + kOutSize, s.out); wr(mdp, b + kOutXy, s.outxy); wr(mdp, b + kStride0, s.stride0);
    wr(mdp, b + kStride1, s.stride1); wr(mdp, b + kFormat, s.format); wr(mdp, b + kUnpack, s.unpack);
    wr(mdp, b + kOpMode, s.opmode); wr(mdp, b + kConstant, s.constant);
}
// CTL 0 flush of the selected pipes (VIG0 bit 0, VIG1 bit 1). Returns the
// microseconds until the flush bits clear, 0xffffffff when still set after 100 ms.
uint32_t flushPipes(volatile uint32_t *mdp, uint32_t mask) {
    wr(mdp, kCtl[0] + kCtlFlush, mask);
    const uint64_t began = mach_absolute_time();
    for (unsigned spin = 0; spin < 2000; ++spin) {
        if (!(rd(mdp, kCtl[0] + kCtlFlush) & mask)) {
            uint64_t ns = 0;
            absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
            return static_cast<uint32_t>(ns / 1000);
        }
        IODelay(50);
    }
    return 0xffffffffU;
}
bool fillPattern(volatile void *bytes, uint32_t width, uint32_t height, uint32_t stride, unsigned mode) {
    if (!bytes) return false;
    volatile uint32_t *pixels = static_cast<volatile uint32_t *>(bytes);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t pixel;
            if (mode == 5) pixel = 0xffff0000U;                  // opaque red (BGRA bytes 00 00 ff ff)
            else if (mode == 6) pixel = 0xff00ff00U;             // opaque green
            else if (mode == 7) pixel = 0xff0000ffU;             // opaque blue
            else if (mode == 8) pixel = ((x ^ y) & 0x40) ? 0xffffffffU : 0xff000000U;  // 64 px checker
            else {
                const uint32_t r = (x * 255) / (width ? width : 1);
                const uint32_t g = (y * 255) / (height ? height : 1);
                const uint32_t b = (x ^ y) & 0xff;
                pixel = 0xff000000U | (r << 16) | (g << 8) | b;
            }
            pixels[(static_cast<uint64_t>(y) * (stride / 4)) + x] = pixel;
        }
    }
    return true;
}

// C2 level 3: a 3200x2136 BGRA gradient in kernel memory, mapped into a slot of
// the display bank's stage-1 window (ShengUfs native_ufs_dpu_map).
bool patternThroughWindow(volatile uint32_t *mdp, const uint32_t *pipes, unsigned count) {
    if (!&native_ufs_dpu_map || !&native_ufs_dpu_state || !(native_ufs_dpu_state >> 31)) {
        IOLog("UKE DPU: test=3 skip reason=window state=%08x\n", &native_ufs_dpu_state ? native_ufs_dpu_state : 0U);
        return false;
    }
    const uint64_t bytes = (uint64_t)kPatternStride * kPatternHeight;
    IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, static_cast<vm_size_t>(bytes), 4096);
    if (!buffer) {
        IOLog("UKE DPU: test=3 skip reason=buffer\n");
        return false;
    }
    if (!fillPattern(buffer->getBytesNoCopy(), kPatternWidth, kPatternHeight, kPatternStride, 4)) {
        buffer->release();
        IOLog("UKE DPU: test=3 skip reason=fill\n");
        return false;
    }
    cacheCleanInvalidate(buffer->getBytesNoCopy(), bytes);
    const unsigned maxPages = static_cast<unsigned>(bytes / 4096 + 1);
    uint64_t *pages = static_cast<uint64_t *>(IOMallocData(maxPages * sizeof(uint64_t)));
    if (!pages) {
        buffer->release();
        IOLog("UKE DPU: test=3 skip reason=pages\n");
        return false;
    }
    unsigned pageCount = 0;
    bool prepared = buffer->prepare() == kIOReturnSuccess;
    bool ok = prepared;
    for (IOByteCount offset = 0; ok && offset < bytes;) {
        IOByteCount length = 0;
        const addr64_t pa = buffer->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);
        if (!pa || (pa & 0xfff) || !length) {ok = false; break;}
        for (IOByteCount at = 0; at < length && offset + at < bytes; at += 4096) pages[pageCount++] = pa + at;
        offset += length;
    }
    uint32_t iova = 0;
    if (ok) iova = native_ufs_dpu_map(0, pages, pageCount);
    if (!ok || !iova) {
        if (prepared) buffer->complete();
        IOFreeData(pages, maxPages * sizeof(uint64_t));
        buffer->release();
        IOLog("UKE DPU: test=3 skip reason=map ok=%u pages=%u iova=%08x\n", ok, pageCount, iova);
        return false;
    }
    SavedPipe saved[2];
    for (unsigned i = 0; i < count; ++i) saved[i] = savePipe(mdp, pipes[i]);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t b = kVig[pipes[i]];
        wr(mdp, b + kSrc0, iova);
        wr(mdp, b + kStride0, kPatternStride & 0xffff);
        wr(mdp, b + kFormat, rd(mdp, b + kFormat) & ~(3U << 30));
        wr(mdp, b + kOpMode, rd(mdp, b + kOpMode) & ~1U);
    }
    const uint32_t latch = flushPipes(mdp, count > 1 ? 0x3 : (1U << pipes[0]));
    IOLog("UKE DPU: test=3 iova=%08x pages=%u bytes=%llu latch_us=%u\n",
          iova, pageCount, (unsigned long long)bytes, latch);
    IOSleep(5000);
    for (unsigned i = 0; i < count; ++i) restorePipe(mdp, pipes[i], saved[i]);
    const uint32_t again = flushPipes(mdp, count > 1 ? 0x3 : (1U << pipes[0]));
    buffer->complete();
    IOFreeData(pages, maxPages * sizeof(uint64_t));
    buffer->release();
    IOLog("UKE DPU: test=3 restored=1 flush=%08x\n", again);
    return true;
}

// C2 level 4: the same picture through the free tail of the splash region
// (below 4 GiB, so the firmware's translation-off bank can read it). The
// geometry shrinks to kSmallWidth x kSmallHeight because the tail is smaller
// than one panel frame.
bool patternThroughLowTail(volatile uint32_t *mdp, const uint32_t *pipes, unsigned count) {
    PE_Video video = {};
    if (PE_current_console(&video) || !video.v_baseAddr || !video.v_rowBytes || !video.v_height) {
        IOLog("UKE DPU: test=4 skip reason=gop\n");
        return false;
    }
    const uint64_t fbBytes = (uint64_t)video.v_rowBytes * video.v_height;
    const uint64_t tail = (uint64_t(video.v_baseAddr) + fbBytes + 0xfffffULL) & ~0xfffffULL;
    const uint64_t bytes = (uint64_t)kSmallWidth * 4 * kSmallHeight;
    if (tail + bytes > kSplashEnd) {
        IOLog("UKE DPU: test=4 skip reason=tail tail=%llx bytes=%llu\n", (unsigned long long)tail, (unsigned long long)bytes);
        return false;
    }
    Map buffer(tail, bytes);
    if (!buffer.va) {
        IOLog("UKE DPU: test=4 skip reason=map tail=%llx\n", (unsigned long long)tail);
        return false;
    }
    const uint32_t sizeValue = (kSmallHeight << 16) | kSmallWidth;
    SavedPipe saved[2];
    for (unsigned i = 0; i < count; ++i) saved[i] = savePipe(mdp, pipes[i]);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t b = kVig[pipes[i]];
        wr(mdp, b + kSrc0, static_cast<uint32_t>(tail));
        wr(mdp, b + kSrcSize, sizeValue);
        wr(mdp, b + kSrcXy, 0);
        wr(mdp, b + kOutSize, sizeValue);
        wr(mdp, b + kOutXy, 0);
        wr(mdp, b + kStride0, (kSmallWidth * 4) & 0xffff);
        wr(mdp, b + kFormat, rd(mdp, b + kFormat) & ~(3U << 30));
        wr(mdp, b + kOpMode, rd(mdp, b + kOpMode) & ~1U);
    }
    // test=9 sweeps the C3 patterns red -> green -> blue -> checker, 2 s each; 4..8 show one.
    const unsigned first = gTestLevel == 9 ? 5 : gTestLevel, last = gTestLevel == 9 ? 8 : gTestLevel;
    for (unsigned mode = first; mode <= last; ++mode) {
        fillPattern(buffer.va, kSmallWidth, kSmallHeight, kSmallWidth * 4, mode);
        __asm__ volatile("dsb sy" ::: "memory");
        const uint32_t latch = flushPipes(mdp, count > 1 ? 0x3 : (1U << pipes[0]));
        IOLog("UKE DPU: test=%u phys=%llx src0=%08x bytes=%llu latch_us=%u\n", mode,
              (unsigned long long)tail, rd(mdp, kVig[pipes[0]] + kSrc0), (unsigned long long)bytes, latch);
        IOSleep(2000);
    }
    for (unsigned i = 0; i < count; ++i) restorePipe(mdp, pipes[i], saved[i]);
    const uint32_t again = flushPipes(mdp, count > 1 ? 0x3 : (1U << pipes[0]));
    IOLog("UKE DPU: test=%u restored=1 flush=%08x\n", gTestLevel, again);
    return true;
}

// C3 control: paint the first half of the GOP in place and leave every DPU register alone.
// If the panel shows the colours, the pipe, format and memory path are sound and the
// gray of the redirected tests is a geometry/address problem of the test buffer.
bool patternOnGop(volatile uint32_t *mdp) {
    (void)mdp;
    PE_Video video = {};
    if (PE_current_console(&video) || !video.v_baseAddr || !video.v_rowBytes || !video.v_height) {
        IOLog("UKE DPU: test=11 skip reason=gop\n");
        return false;
    }
    const uint64_t bytes = (uint64_t)video.v_rowBytes * video.v_height;
    Map buffer(video.v_baseAddr, bytes);
    if (!buffer.va) {
        IOLog("UKE DPU: test=11 skip reason=map\n");
        return false;
    }
    for (unsigned mode = 5; mode <= 8; ++mode) {
        fillPattern(buffer.va, video.v_width / 2, video.v_height, video.v_rowBytes, mode);
        __asm__ volatile("dsb sy" ::: "memory");
        IOLog("UKE DPU: test=11 mode=%u base=%llx w=%u h=%u stride=%u\n", mode,
              (unsigned long long)video.v_baseAddr, (unsigned)(video.v_width / 2),
              (unsigned)video.v_height, (unsigned)video.v_rowBytes);
        IOSleep(2000);
    }
    IOLog("UKE DPU: test=11 done\n");
    return true;
}

// C3 control 12: redirect the pipes to a sub-region of the GOP itself (known display-readable),
// keeping the live row pitch and half-panel width, shrinking only the height so it fits. If this
// shows colours, the gray of tests 4/9 is the low-tail memory, not the geometry change.
bool patternInGopWindow(volatile uint32_t *mdp, const uint32_t *pipes, unsigned count) {
    PE_Video video = {};
    if (PE_current_console(&video) || !video.v_baseAddr || !video.v_rowBytes || !video.v_height) {
        IOLog("UKE DPU: test=12 skip reason=gop\n");
        return false;
    }
    const uint32_t width = video.v_width / 2, height = 1068;
    const uint64_t base = (uint64_t)video.v_baseAddr + 0x800000ULL;
    const uint64_t bytes = (uint64_t)video.v_rowBytes * height;
    if (base + bytes > (uint64_t)video.v_baseAddr + (uint64_t)video.v_rowBytes * video.v_height) {
        IOLog("UKE DPU: test=12 skip reason=range\n");
        return false;
    }
    Map buffer(base, bytes);
    if (!buffer.va) {
        IOLog("UKE DPU: test=12 skip reason=map\n");
        return false;
    }
    const uint32_t sizeValue = (height << 16) | width;
    SavedPipe saved[2];
    for (unsigned i = 0; i < count; ++i) saved[i] = savePipe(mdp, pipes[i]);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t b = kVig[pipes[i]];
        wr(mdp, b + kSrc0, static_cast<uint32_t>(base));
        wr(mdp, b + kSrcSize, sizeValue);
        wr(mdp, b + kSrcXy, 0);
        wr(mdp, b + kOutSize, sizeValue);
        wr(mdp, b + kOutXy, i ? (width << 16) : 0);
        wr(mdp, b + kStride0, video.v_rowBytes & 0xffff);
        wr(mdp, b + kFormat, rd(mdp, b + kFormat) & ~(3U << 30));
        wr(mdp, b + kOpMode, rd(mdp, b + kOpMode) & ~1U);
    }
    for (unsigned mode = 5; mode <= 8; ++mode) {
        fillPattern(buffer.va, width, height, video.v_rowBytes, mode);
        __asm__ volatile("dsb sy" ::: "memory");
        const uint32_t latch = flushPipes(mdp, count > 1 ? 0x3 : (1U << pipes[0]));
        IOLog("UKE DPU: test=12 mode=%u phys=%llx src0=%08x w=%u h=%u latch_us=%u\n", mode,
              (unsigned long long)base, rd(mdp, kVig[pipes[0]] + kSrc0), width, height, latch);
        IOSleep(2000);
    }
    for (unsigned i = 0; i < count; ++i) restorePipe(mdp, pipes[i], saved[i]);
    const uint32_t again = flushPipes(mdp, count > 1 ? 0x3 : (1U << pipes[0]));
    IOLog("UKE DPU: test=12 restored=1 flush=%08x\n", again);
    return true;
}

void test(volatile uint32_t *mdp) {
    if (gTestLevel == 11) {patternOnGop(mdp); return;}
    uint32_t pipes[2] = {};
    const unsigned count = livePipes(mdp, pipes);
    if (!count) {
        IOLog("UKE DPU: test=%u skip reason=nopipe\n", gTestLevel);
        return;
    }
    uint32_t mask = 0;
    for (unsigned i = 0; i < count; ++i) mask |= 1U << pipes[i];
    if (gTestLevel == 1) {
        // Write back each live address untouched: the write path and the latch,
        // no picture change.
        for (unsigned i = 0; i < count; ++i) wr(mdp, kVig[pipes[i]] + kSrc0, rd(mdp, kVig[pipes[i]] + kSrc0));
        const uint32_t latch = flushPipes(mdp, mask);
        IOLog("UKE DPU: test=1 pipe=%u flush=%08x latch_us=%u final=%08x\n",
              count, mask, latch, rd(mdp, kCtl[0] + kCtlFlush));
    } else if (gTestLevel == 2) {
        SavedPipe saved[2];
        for (unsigned i = 0; i < count; ++i) saved[i] = savePipe(mdp, pipes[i]);
        for (unsigned i = 0; i < count; ++i) {
            const uint32_t b = kVig[pipes[i]];
            wr(mdp, b + kConstant, 0xffff0000U);                     // opaque red, ABGR8888 order
            wr(mdp, b + kFormat, (rd(mdp, b + kFormat) & ~(3U << 30)) | (1U << 22));
        }
        const uint32_t latch = flushPipes(mdp, mask);
        IOLog("UKE DPU: test=2 pipe=%u color=ffff0000 latch_us=%u\n", count, latch);
        IOSleep(5000);
        for (unsigned i = 0; i < count; ++i) restorePipe(mdp, pipes[i], saved[i]);
        const uint32_t again = flushPipes(mdp, mask);
        IOLog("UKE DPU: test=2 restored=1 flush=%08x\n", again);
    } else if (gTestLevel == 3) {
        patternThroughWindow(mdp, pipes, count);
    } else if (gTestLevel == 12) {
        patternInGopWindow(mdp, pipes, count);
    } else if ((gTestLevel >= 4 && gTestLevel <= 8) || gTestLevel == 9) {
        patternThroughLowTail(mdp, pipes, count);
    } else {
        IOLog("UKE DPU: test=%u skip reason=level\n", gTestLevel);
    }
}

void dpuThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "uke-dpu");
    unsigned probeDelay = 8, testDelay = 20;
    PE_parse_boot_argn("uke-dpu-delay", &probeDelay, sizeof(probeDelay));
    PE_parse_boot_argn("uke-dpu-test-delay", &testDelay, sizeof(testDelay));
    if (probeDelay > 600) probeDelay = 600;
    IOSleep((uint64_t)probeDelay * 1000);
    Map mdp(kMdpBase, kMdpBytes);
    if (!mdp.va) {
        IOLog("UKE DPU: probe fail stage=map\n");
        return;
    }
    if (gProbeLevel) snapshot();
    if (gTestLevel) {
        if (testDelay > probeDelay) IOSleep((uint64_t)(testDelay - probeDelay) * 1000);
        native_dpu_stage = 10 + gTestLevel;
        test(mdp.va);
        native_dpu_stage = 20;
    }
}

}  // namespace

extern "C" volatile uint32_t native_dpu_stage = 0;

extern "C" void native_dpu_probe(void) {
    unsigned probe = 0, test = 0;
    PE_parse_boot_argn("uke-dpu", &probe, sizeof(probe));
    PE_parse_boot_argn("uke-dpu-test", &test, sizeof(test));
    if (probe > 2) probe = 2;
    if (test > 12) test = 12;
    if ((!probe && !test) || native_dpu_stage) return;
    gProbeLevel = probe;
    gTestLevel = test;
    native_dpu_stage = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(dpuThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_dpu_stage = 0xffffffffU;
}
