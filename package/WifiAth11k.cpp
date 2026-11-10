// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board platform for the ported ath11k AHB stack (uke-wifi=3).
// The driver owns QMI/WMI/mac80211; this file provides the lx_platform
// callbacks, the persistent BAR mapping, the GLINK QRTR endpoint, the MSA
// staging buffer and the WPSS start hand-off.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <mach/kmod.h>
#include <stdarg.h>
#include <string.h>
#include "UkeClassRegistration.h"
extern "C" {
#include "nw_abi.h"
#include "nw_control_endpoint.h"
#include "nw_wcn6750_qmi.h"
extern const struct lx_platform *lx_plat;
// Set by nw_wifi_start(); the board's MSI bitmap only becomes meaningful once
// the endpoint is running, but the poll can be called before that.
extern const struct nw_board *nw_board;
int uke_wpss_send(void *channel, const void *data, uint32_t length);
int uke_wlfw_load_stock_data(unsigned type, uint8_t **data, uint32_t *length);
void uke_wpss_glink_poll(void);
extern void *gAhbGlChannel;
extern unsigned int ath11k_debug_mask;
void lx_irq_raise(unsigned int irq);
void uke_wifi_ahb_wake(void);
// The AHB platform provider exposes the BAR mapping it hands the driver; the
// lifecycle (nw_wifi_start/step), the board description and the diagnostics are
// the module's own (nw_wifi.c and nw_diag_ahb.c, built for NW_PLATFORM_AHB).
void *nw_board_bar(void);
// Ring/queue inventory. nw_abi.h only publishes the transmit-completion and
// link lines WifiNet consumes; the board's progress line reads these two.
void nw_wifi_ring_state(char *text, int size);
void nw_wifi_rx_state(char *text, int size);
// The kernel's own preemption counter pair (out-of-line globals in the image:
// 0xfffffff00af21308 / 0xfffffff00af2144c in the 24A446 kernel). The driver
// guards its shared boundary and control spinlocks with them through
// lx_shared_spin_*, including the control socket's queue on this side.
void _disable_preemption(void);
void _enable_preemption(void);
// Station layer (nw_mainline.c): the default interface and the self-test scan.
struct net_device *nw_cfg80211_primary(void);
int nw_cfg80211_selftest_scan(void);
// ShengUfs builds this window at UFS init: a context bank whose stage 1 maps
// native_ufs_extra_iova onto one contiguous buffer, so the endpoint reaches
// every DMA allocation with a 32-bit IOVA (its stream must select that bank).
extern uint8_t *native_ufs_extra_va __attribute__((weak_import));
extern volatile uint32_t native_ufs_extra_iova __attribute__((weak_import));
extern volatile uint32_t native_ufs_extra_bytes __attribute__((weak_import));
extern volatile uint32_t native_ufs_extra_uncached __attribute__((weak_import));
extern volatile uint32_t native_ufs_msi_iova __attribute__((weak_import));
}

namespace {
constexpr uint64_t kWlanBarBase = 0x61e00000ULL, kWlanBarBytes = 0x200000ULL;
constexpr uint64_t kMsaBase = 0xa6400000ULL, kMsaBytes = 0x00c00000ULL;
struct Mapping {uint64_t base, bytes; IOMemoryMap *map; volatile uint8_t *va;};
Mapping gMappings[8];
unsigned gMappingCount;

void *mapPersistent(uint64_t pa, size_t bytes) {
    for (unsigned i = 0; i < gMappingCount; ++i) {
        const Mapping &m = gMappings[i];
        if (pa >= m.base && pa + bytes <= m.base + m.bytes) return const_cast<uint8_t *>(m.va) + (pa - m.base);
    }
    if (gMappingCount >= 8) return nullptr;
    const uint64_t base = pa & ~0xfffULL, offset = pa - base;
    const uint64_t span = (offset + bytes + 0xfff) & ~0xfffULL;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, span);
    if (!memory) return nullptr;
    IOMemoryMap *map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (!map) return nullptr;
    Mapping &m = gMappings[gMappingCount++];
    m.base = base; m.bytes = span; m.map = map;
    m.va = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
    return const_cast<uint8_t *>(m.va) + offset;
}

// Reuse sheng WifiProbe's split-and-coalesce allocator in the endpoint's
// SMMU window. Allocation and release run on the LX worker; the hardware
// interrupt handler only raises an LX interrupt.
constexpr unsigned kDmaBlocks = 16384;
struct Arena {
    uint8_t *va;
    uint64_t iova;
    size_t bytes;
    struct Block {uint32_t off, size; bool used;} *blocks;
    unsigned count;
} gArena;

bool windowInit() {
    if (gArena.blocks) return true;
    if (&native_ufs_extra_va == nullptr || &native_ufs_extra_iova == nullptr ||
        &native_ufs_extra_bytes == nullptr || &native_ufs_extra_uncached == nullptr ||
        !native_ufs_extra_va || !native_ufs_extra_iova || !native_ufs_extra_bytes ||
        !native_ufs_extra_uncached ||
        uint64_t(native_ufs_extra_iova) + native_ufs_extra_bytes > (1ULL << 32)) return false;

    gArena.blocks = static_cast<Arena::Block *>(IOMalloc(kDmaBlocks * sizeof(Arena::Block)));
    if (!gArena.blocks) return false;
    gArena.va = native_ufs_extra_va;
    gArena.iova = native_ufs_extra_iova;
    gArena.bytes = native_ufs_extra_bytes;
    gArena.blocks[0] = {0, native_ufs_extra_bytes, false};
    gArena.count = 1;
    printf("UKE WIFI AHB: uncached DMA window iova=%08llx bytes=%lu\n",
           (unsigned long long)gArena.iova, (unsigned long)gArena.bytes);
    return true;
}

bool inWindow(const void *p, size_t n) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(gArena.va);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    return start && addr >= start && addr - start < gArena.bytes &&
           n <= gArena.bytes - (addr - start);
}

void *dmaAlloc(size_t n, uint64_t *bus) {
    if (!n || n > gArena.bytes) return nullptr;
    n = (n + 63) & ~(size_t)63;
    uint64_t align = 64;
    if (n >= 4096) {
        n = (n + 4095) & ~(size_t)4095;
        align = 4096;
        while (align < n) align <<= 1;
    }
    for (unsigned i = 0; i < gArena.count; ++i) {
        const Arena::Block b = gArena.blocks[i];
        if (b.used) continue;
        const uint64_t iova = gArena.iova + b.off;
        const uint32_t pad = ((iova + align - 1) & ~(align - 1)) - iova;
        const uint32_t start = b.off + pad;
        if (pad > b.size || n > b.size - pad) continue;
        const bool tail = b.size > pad + n;
        const unsigned extra = unsigned(pad != 0) + unsigned(tail);
        if (gArena.count + extra > kDmaBlocks) continue;
        if (extra && i + 1 < gArena.count)
            memmove(&gArena.blocks[i + extra + 1], &gArena.blocks[i + 1],
                    (gArena.count - i - 1) * sizeof(Arena::Block));
        gArena.count += extra;
        unsigned at = i;
        const uint32_t size = static_cast<uint32_t>(n);
        if (pad) gArena.blocks[at++] = {b.off, pad, false};
        gArena.blocks[at++] = {start, size, true};
        if (tail) gArena.blocks[at] = {start + size, b.size - pad - size, false};
        uint8_t *p = gArena.va + start;
        bzero(p, n);
        if (bus) *bus = gArena.iova + start;
        return p;
    }
    static unsigned failures;
    if (failures++ < 4)
        printf("UKE WIFI AHB: DMA window exhausted want=%lu blocks=%u\n",
               (unsigned long)n, gArena.count);
    return nullptr;
}

void dmaFree(size_t n, void *p, uint64_t bus) {
    if (!inWindow(p, n)) return;
    const uint32_t off = (uint32_t)((uint8_t *)p - gArena.va);
    // lx_dma_kfree supplies just the allocation pointer.
    if (bus && bus != gArena.iova + off) return;
    for (unsigned i = 0; i < gArena.count; ++i) {
        if (gArena.blocks[i].off != off || !gArena.blocks[i].used || n > gArena.blocks[i].size) continue;
        gArena.blocks[i].used = false;
        if (i + 1 < gArena.count && !gArena.blocks[i + 1].used) {
            gArena.blocks[i].size += gArena.blocks[i + 1].size;
            memmove(&gArena.blocks[i + 1], &gArena.blocks[i + 2],
                    (gArena.count - i - 2) * sizeof(Arena::Block));
            --gArena.count;
        }
        if (i > 0 && !gArena.blocks[i - 1].used) {
            gArena.blocks[i - 1].size += gArena.blocks[i].size;
            memmove(&gArena.blocks[i], &gArena.blocks[i + 1],
                    (gArena.count - i - 1) * sizeof(Arena::Block));
            --gArena.count;
        }
        return;
    }
}

uint64_t nowMs(void) {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
uint64_t nowNs(void) {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns;}
void sleepMs(unsigned ms) {IOSleep(ms);}

constexpr uint32_t kMsiSpiBase = 0x300;       // DT SPI number of vector 0
// armcpu_spi_attach() takes the GIC INTID, which for "GIC_SPI n" is n + 32. The OEM
// qcom,pci-msi driver programs msg->data = irqd_to_hwirq(parent) = 0x320 + n, i.e. the same
// INTID, and that is what the endpoint writes into the doorbell.
constexpr uint32_t kMsiIntidBase = kMsiSpiBase + 32;
uint32_t gMsiDoorbellIova;
unsigned gSpiCount[32];
uint32_t gMsiPending;
extern "C" int armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context,
                                 uint32_t *readback, uint64_t *router) __attribute__((weak_import));

// The reference DTS declares these interrupts as edge triggered (type 1), one per vector:
// the SPI number identifies the vector, so the host never has to read or clear the doorbell
// register, which this CPU cannot even map (kernel data abort, esr 0x96000006). Dispatch from
// the SPI number alone and let the GIC do the bookkeeping.
// Runs with interrupts disabled: only counters, atomic pending bits and worker wake
// (kprintf from that context panics: "console I/O from interrupt-disabled context").
void msiSpiVector(void *context) {
    const unsigned vector = (unsigned)(uintptr_t)context;
    __atomic_fetch_add(&gSpiCount[vector], 1u, __ATOMIC_RELAXED);
    __atomic_fetch_or(&gMsiPending, 1u << vector, __ATOMIC_RELEASE);
    uke_wifi_ahb_wake();
}

bool msiSpiAttach() {
    if (&native_ufs_msi_iova == nullptr || !native_ufs_msi_iova || !armcpu_spi_attach) {
        printf("UKE WIFI AHB: hardware MSI unavailable\n");
        return false;
    }
    gMsiDoorbellIova = native_ufs_msi_iova;
    printf("UKE WIFI AHB: msi doorbell iova=%08x (pa 0x17110040)\n", gMsiDoorbellIova);
    unsigned ok = 0;
    for (unsigned vector = 0; vector < 32; ++vector) {
        // The reference DTS declares these as IRQ_TYPE_EDGE_RISING; a level configuration
        // storms because the doorbell page cannot be cleared from this CPU.
        const int rc = armcpu_spi_attach(kMsiIntidBase + vector, true, msiSpiVector,
                                         (void *)(uintptr_t)vector, nullptr, nullptr);
        if (rc == 0) ++ok;
        else printf("UKE WIFI AHB: intid %u attach rc %d\n", kMsiIntidBase + vector, rc);
    }
    printf("UKE WIFI AHB: msi spi attach %u/32\n", ok);
    return ok == 32;
}

// sheng hands the atomic MSI bitmap to the LX dispatcher on its worker. The
// module owns the vector -> IRQ translation (nw_board.irq_base), so the board
// only supplies the bitmap; before nw_wifi_start() the pointer is null and the
// bits are translated here.
uint64_t gPollCalls, gPollGapMaxMs, gStepMaxMs;
uint32_t gLogSuppressed;
void pollGl() {
    const uint64_t now = nowMs();
    static uint64_t last;
    if (last && now - last > gPollGapMaxMs) gPollGapMaxMs = now - last;
    last = now;
    ++gPollCalls;
    uint32_t pending = __atomic_exchange_n(&gMsiPending, 0u, __ATOMIC_ACQ_REL);
    if (nw_board) nw_wifi_irq_pending(pending);
    else for (unsigned vector = 0; pending; ++vector, pending >>= 1)
        if (pending & 1u) lx_irq_raise(0x400 + vector);
    // Same handoff as sheng's pPoll(): synchronous firmware waits pump here
    // before returning to nw_wifi_step(). Admit newly received control/data
    // work on every pump; the dispatcher still owns execution and lock gating.
    nw_wifi_poll();
    uke_wpss_glink_poll();
}
bool logHas(const char *line, const char *text) {
    for (; *line; ++line) {
        size_t i = 0;
        while (text[i] && line[i] == text[i]) ++i;
        if (!text[i]) return true;
    }
    return false;
}
void logLine(int level, const char *line) {
    // sheng's pLog filters per-channel/per-buffer debug and repeated lines.
    // ath11k spells the HTC RX and foreign-channel messages differently.
    // Never filter warnings/errors or the native control/SME observations.
    if (level >= 7) {
        if (logHas(line, "chan scan list") || logHas(line, "mac channel [") ||
            logHas(line, "htc rx completion") || logHas(line, "htc rx ep ") ||
            logHas(line, "rx ce pipe") || logHas(line, "ext irq:") ||
            logHas(line, "event mgmt rx") || logHas(line, "mgmt rx event") ||
            logHas(line, "chan info vdev_id") || logHas(line, "scan event foreign channel") ||
            logHas(line, "event scan foreign channel") || !strcmp(line, "wmi event diag")) {
            ++gLogSuppressed;
            return;
        }
        static uint32_t recent[16];
        uint32_t hash = 2166136261u;
        for (const char *p = line; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
        for (uint32_t previous : recent) if (previous == hash) {++gLogSuppressed; return;}
        memmove(&recent[1], &recent[0], sizeof recent - sizeof recent[0]);
        recent[0] = hash;
    }
    printf("UKE WIFI AHB: %s\n", line);
}
void haltNoop(void) {}
void *allocHdr(size_t n) {
    uint8_t *b = (uint8_t *)IOMalloc(n + 16);
    if (!b) return nullptr;
    *(size_t *)b = n;
    return b + 16;
}
void freeHdr(void *p) {
    if (!p) return;
    uint8_t *b = (uint8_t *)p - 16;
    IOFree(b, *(size_t *)b + 16);
}
uint64_t dmaMap(void *p, size_t n, int) {
    return inWindow(p, n) ? gArena.iova + (static_cast<uint8_t *>(p) - gArena.va) : 0;
}
// The board requires ShengUfs's uncached alias for both coherent descriptors
// and streaming buffers. The LX ring code supplies ordering barriers.
void dmaSync(uint64_t, size_t, int) {}
void busyDelay(unsigned us) {IODelay(us);}
void preemptOff(void) {_disable_preemption();}
void preemptOn(void) {_enable_preemption();}

// The stock partition holds exactly two WiFi assets: the OEM icnss2 board-data
// ELF chosen for the measured CN O82 board (bd_o82.elf, the raw API-1
// board.bin layout) and the Xiaomi regulatory database. The firmware itself is
// started from that partition by the board through WPSS PAS, so ath11k never
// asks for amss.bin or m3.bin here.
//
// The names below are what the D61F boot actually requested: board-2.bin (the
// API-2 container format) three times, then board.bin, then regdb.bin. Answering
// board-2.bin with the ELF is what made the driver print "found invalid board
// magic" before falling back; withholding it makes the fallback explicit, and
// keeps every other name (firmware-2.bin, wlan_mac.bin, ...) from receiving
// board data it cannot parse.
enum { kStockNone, kStockRegdb, kStockBdf };
int stockAsset(const char *name) {
    const char *base = name;
    for (const char *p = name; *p; ++p) if (*p == '/') base = p + 1;
    if (!strcmp(base, "regdb.bin") || !strcmp(base, "regdb_xiaomi.bin")) return kStockRegdb;
    if (!strcmp(base, "board.bin")) return kStockBdf;
    return kStockNone;
}
const char *stockSource(int asset) {return asset == kStockRegdb ? "regdb_xiaomi.bin" : "bd_o82.elf";}
struct StockSlot {const uint8_t *data; size_t length;};
StockSlot gStock[3];  // indexed by the asset enum: kStockNone is never filled
// One line per distinct request name: the file the stack asked for and what the
// board answered. A new name here means a driver change, and a name that stops
// being logged means the driver stopped asking for it.
void noteRequest(const char *base, const char *answer) {
    static char seen[8][32];
    static unsigned count;
    for (unsigned i = 0; i < count; ++i) if (!strcmp(seen[i], base)) return;
    if (count == 8) return;
    strlcpy(seen[count++], base, sizeof seen[0]);
    printf("UKE WIFI AHB: fw request %s -> %s\n", base, answer);
}
int fwGet(const char *name, const uint8_t **data, size_t *size) {
    if (!name || !data || !size) return -EINVAL;
    char base[32];
    const char *slash = name;
    for (const char *p = name; *p; ++p) if (*p == '/') slash = p + 1;
    strlcpy(base, slash, sizeof base);
    const int asset = stockAsset(base);
    if (asset == kStockNone) {noteRequest(base, "not on this device"); return -ENOENT;}
    if (!gStock[asset].data) {
        uint8_t *p = nullptr; uint32_t length = 0;
        const unsigned type = asset == kStockRegdb ? NW_WCN6750_REGDB : NW_WCN6750_BDF_ELF;
        if (uke_wlfw_load_stock_data(type, &p, &length) || !p || !length) {
            noteRequest(base, "stock asset unreadable");
            return -ENOENT;
        }
        gStock[asset].data = p; gStock[asset].length = length;
    }
    *data = gStock[asset].data; *size = gStock[asset].length;
    // The loader already logged "verified stock <path> bytes=<n>" on its first
    // read; this line names the stack's request and the asset that answered it.
    noteRequest(base, stockSource(asset));
    return 0;
}

const struct lx_platform gPlatform = {
    nowMs, sleepMs, pollGl, logLine, haltNoop, allocHdr, freeHdr,
    dmaAlloc, dmaFree, dmaMap, dmaSync, fwGet, nowNs, nowNs, busyDelay,
    preemptOff, preemptOn,
};
}  // namespace

extern "C" void *nw_board_map_phys(uint64_t pa, size_t bytes) {return mapPersistent(pa, bytes);}
extern "C" uint64_t nw_board_msi_iova(void) {return gMsiDoorbellIova;}
// Per-vector interrupt counts and the vectors seen so far, for the board's
// progress line: a vector that stops arriving is a stuck ring, not silence.
extern "C" void uke_wifi_msi_stats(unsigned *counts, unsigned *vectors) {
    unsigned mask = 0;
    for (unsigned i = 0; i < 32; ++i) {
        counts[i] = __atomic_load_n(&gSpiCount[i], __ATOMIC_RELAXED);
        if (counts[i]) mask |= 1u << i;
    }
    *vectors = mask;
}
extern "C" void *nw_board_bar(void) {return mapPersistent(kWlanBarBase, kWlanBarBytes);}
extern "C" int nw_board_mac(unsigned char *address) {
    static const unsigned char mac[6] = {0x02, 0x00, 0x00, 0x75, 0x67, 0x50};
    if (!address) return -EINVAL;
    for (unsigned i = 0; i < 6; ++i) address[i] = mac[i];
    return 0;
}
extern "C" int nw_board_msa_write(uint64_t offset, const void *data, uint32_t length) {
    if (offset + length > kMsaBytes) return -1;
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(mapPersistent(kMsaBase, kMsaBytes));
    if (!base) return -2;
    const volatile uint8_t *src = static_cast<const volatile uint8_t *>(data);
    for (uint32_t i = 0; i < length; ++i) base[offset + i] = src[i];
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t src_sum = 0, dst_sum = 0;
    for (uint32_t i = 0; i < length; ++i) {src_sum = src_sum * 31 + src[i]; dst_sum = dst_sum * 31 + base[offset + i];}
    uint32_t first = 0;
    for (unsigned i = 0; i < 4 && i < length; ++i) first |= (uint32_t)base[offset + i] << (8 * i);
    printf("UKE WIFI AHB: MSA stage len=%u src=%08x dst=%08x first=%08x\n", length, src_sum, dst_sum, first);
    return src_sum == dst_sum ? 0 : -3;
}
extern "C" int nw_board_glink_xmit(const void *packet, size_t length) {return uke_wpss_send(gAhbGlChannel, packet, (uint32_t)length);}
extern "C" int nw_board_wpss_start(void) {return 0;}  // the WPSS worker already started it

// ---- native control socket: org.unixnu.wifi.control ------------------------
// Ported from sheng's WifiProbe.cpp (controlConnect..controlStart). nw_control.c
// owns the queues and the protocol; this side is the XNU kernel-control socket
// and the transport (send/wake) it was missing. One client owns one session;
// wpa_supplicant becomes the real client in the next stage, and the board runs
// a single self-test session before that so queue -> dispatch -> publish -> send
// is proven without userspace.
struct ControlClient {uint64_t session; uint32_t unit; unsigned rxShown, dcShown;};
kern_ctl_ref gControlRef;
IOLock *gControlLock;
ControlClient *gControlClient;
uint64_t gSelfTestSession;
bool gSelfTestReply;

errno_t controlError(int status) {
    switch (status) {
        case NW_CONTROL_OK: return 0;
        case NW_CONTROL_INVALID: return EINVAL;
        case NW_CONTROL_BUSY: return EWOULDBLOCK;
        case NW_CONTROL_NOT_READY: return ENXIO;
        case NW_CONTROL_AUTH_FAILED: return EACCES;
        case NW_CONTROL_TIMED_OUT: return ETIMEDOUT;
        case NW_CONTROL_UNSUPPORTED: return EOPNOTSUPP;
        case NW_CONTROL_NO_MEMORY: return ENOBUFS;
        case NW_CONTROL_NOT_FOUND: return ENOENT;
        case NW_CONTROL_STALE: return ESTALE;
        default: return EIO;
    }
}
errno_t controlConnect(kern_ctl_ref, sockaddr_ctl *address, void **information) {
    // Print before attaching: a BUSY attach is the interesting case and must be
    // visible in the boot ring.
    printf("UKE WIFI AHB: control connect entry unit %u\n", address ? address->sc_unit : 0);
    auto *owner = static_cast<ControlClient *>(IOMalloc(sizeof(ControlClient)));
    if (!owner) return ENOMEM;
    const int status = nw_wifi_control_attach(&owner->session);
    if (status) {
        printf("UKE WIFI AHB: control connect attach failed %d\n", status);
        IOFree(owner, sizeof(*owner)); return controlError(status);
    }
    owner->unit = address->sc_unit;
    owner->rxShown = 0; owner->dcShown = 0;
    IOLockLock(gControlLock); gControlClient = owner; IOLockUnlock(gControlLock);
    printf("UKE WIFI AHB: control connect unit %u session %llu\n", owner->unit, owner->session);
    *information = owner; return 0;
}
errno_t controlDisconnect(kern_ctl_ref, uint32_t, void *information) {
    // XNU invokes disconnect after a failed connect, with no client context.
    if (!information) return 0;
    auto *owner = static_cast<ControlClient *>(information);
    // Per-session bound: 48 records per session, not 32 across the whole boot.
    if (owner->dcShown++ < 48) printf("UKE WIFI CTL: disconnect session %llu\n", owner->session);
    nw_wifi_control_detach(owner->session);
    IOLockLock(gControlLock); if (gControlClient == owner) gControlClient = nullptr; IOLockUnlock(gControlLock);
    IOFree(owner, sizeof(*owner)); return 0;
}
int controlReadMbuf(void *source, void *destination, size_t length) {
    return mbuf_copydata(static_cast<mbuf_t>(source), 0, length, destination);
}
// Per-session bounded tracing. V4.8 used function-static global counters, so
// the first 32 receives / 64 sends (all in session 6, by ~106s/~204s) exhausted
// them and the whole join window looked empty. Counters now belong to the
// client, and the per-op totals below are never bounded: they are printed from
// the board's periodic progress line so a busy join is always observable.
// Key operations: 1 INFO, 2 SCAN, 3 GET_BSS, 4 AUTHENTICATE, 5 ASSOCIATE,
// 7 SET_KEY, 8 SET_PORT, 257 SCAN_STARTED, 258 SCAN_DONE, 264 JOIN_REQUEST,
// 265 DISCONNECT_REQUEST, 267 SCAN_REQUEST, 270 WIRELESS_EVENT.
enum {kCtlOpCount = 14};
static const unsigned kCtlOps[kCtlOpCount] = {1, 2, 3, 4, 5, 7, 8, 257, 258, 264, 265, 267, 270, 0};
static volatile uint32_t gCtlRxOp[kCtlOpCount], gCtlTxOp[kCtlOpCount];
static volatile uint32_t gCtlRxTotal, gCtlTxTotal, gCtlRxLastStatus;
static volatile uint32_t gCtlTxLastSocket, gCtlRcvdTotal;
static void ctlCountOp(volatile uint32_t *table, unsigned op) {
    for (unsigned i = 0; i < kCtlOpCount - 1; i++)
        if (kCtlOps[i] == op) {__atomic_add_fetch(&table[i], 1u, __ATOMIC_RELAXED); return;}
}
errno_t controlReceive(kern_ctl_ref, uint32_t, void *information, mbuf_t packet, int) {
    auto *owner = static_cast<ControlClient *>(information);
    const size_t length = mbuf_pkthdr_len(packet);
    const int status = nw_wifi_control_receive(owner->session, length, controlReadMbuf, packet);
    __atomic_add_fetch(&gCtlRxTotal, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&gCtlRxLastStatus, (uint32_t)status, __ATOMIC_RELAXED);
    nw_control_view view{};
    // The module decodes the full message in nw_wifi_control_receive(); here we
    // only need the 40-byte header for classification, so read it without
    // re-running the codec (which requires the whole message buffer).
    uint8_t head[NW_CONTROL_HEADER_SIZE];
    if (length >= sizeof(head) && !mbuf_copydata(packet, 0, sizeof(head), head)) {
        const uint32_t magic = (uint32_t)head[0] | ((uint32_t)head[1] << 8) |
                               ((uint32_t)head[2] << 16) | ((uint32_t)head[3] << 24);
        if (magic == NW_CONTROL_MAGIC) {
            const unsigned kind = (unsigned)head[6] | ((unsigned)head[7] << 8);
            const unsigned op = (unsigned)head[8] | ((unsigned)head[9] << 8);
            const uint32_t seq = (uint32_t)head[12] | ((uint32_t)head[13] << 8) |
                                 ((uint32_t)head[14] << 16) | ((uint32_t)head[15] << 24);
            uint64_t gen = 0;
            for (unsigned i = 0; i < 8; i++) gen |= (uint64_t)head[32 + i] << (8 * i);
            view.header.kind = (uint16_t)kind; view.header.operation = (uint16_t)op;
            view.header.sequence = seq; view.header.generation = gen;
            ctlCountOp(gCtlRxOp, op);
        }
    }
    // Per-session bound: 64 records per session instead of 32 globally.
    if (owner->rxShown++ < 64)
        printf("UKE WIFI CTL: receive session %llu bytes %zu enqueue %d\n", owner->session, length, status);
    // Join-critical requests are always printed, independent of any bound:
    // AUTHENTICATE(4) / ASSOCIATE(5) / SET_KEY(7) / SET_PORT(8) / REPORT_RESULT(12).
    if (view.header.operation == 4 || view.header.operation == 5 || view.header.operation == 7 ||
        view.header.operation == 8 || view.header.operation == 12)
        printf("UKE WIFI CTL: KEYRX session %llu kind %u op %u seq %u gen %llu len %zu enqueue %d\n",
               owner->session, view.header.kind, view.header.operation, view.header.sequence,
               view.header.generation, length, status);
    mbuf_freem(packet); return controlError(status);
}
void controlReceived(kern_ctl_ref, uint32_t, void *information, int) {
    // ctl_rcvd fires when the userspace client reads enqueued data. Logging it
    // (with a bounded counter) distinguishes "event never read" from "event
    // read but rejected by nw_control_client_event" after a JOIN_REQUEST.
    auto *owner = static_cast<ControlClient *>(information);
    static unsigned shown;
    if (__atomic_add_fetch(&shown, 1u, __ATOMIC_RELAXED) <= 256)
        printf("UKE WIFI CTL: rcvd session %llu\n", owner ? owner->session : 0);
    __atomic_add_fetch(&gCtlRcvdTotal, 1u, __ATOMIC_RELAXED);
    if (owner) nw_wifi_control_writable(owner->session);
}
errno_t controlGetOption(kern_ctl_ref, uint32_t, void *information, int option, void *bytes, size_t *length) {
    if (option != NW_CONTROL_GET_SESSION) return ENOPROTOOPT;
    if (!length) return EINVAL;
    if (!bytes || *length < sizeof(uint64_t)) {*length = sizeof(uint64_t); return EMSGSIZE;}
    const uint64_t session = static_cast<ControlClient *>(information)->session;
    memcpy(bytes, &session, sizeof(session)); *length = sizeof(session); return 0;
}
int controlSend(void *, uint64_t session, const void *bytes, size_t length) {
    if (gSelfTestSession && session == gSelfTestSession) {
        nw_control_view view;
        if (!nw_control_decode(bytes, length, &view)) {
            printf("UKE WIFI AHB: control selftest reply op %u status %d len %zu\n",
                   view.header.operation, view.header.status, length);
            gSelfTestReply = true;
        }
        return NW_CONTROL_SEND_OK;
    }
    IOLockLock(gControlLock);
    const bool active = gControlClient && gControlClient->session == session;
    const uint32_t unit = active ? gControlClient->unit : 0;
    IOLockUnlock(gControlLock);
    if (!active) return NW_CONTROL_SEND_CLOSED;
    // No adapter lock is held while the socket layer takes its own locks.
    const errno_t result = ctl_enqueuedata(gControlRef, unit, const_cast<void *>(bytes), length, CTL_DATA_EOR);
    nw_control_view view{};
    const int decoded = !nw_control_decode(bytes, length, &view);
    __atomic_add_fetch(&gCtlTxTotal, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&gCtlTxLastSocket, (uint32_t)result, __ATOMIC_RELAXED);
    if (decoded) ctlCountOp(gCtlTxOp, view.header.operation);
    // Per-session bound: a new session id resets the counter, so a long-lived
    // INFO session can no longer blind the whole boot. Join-critical events
    // are always printed regardless of the bound.
    static uint64_t lastSession;
    static unsigned txShown;
    if (lastSession != session) {lastSession = session; txShown = 0;}
    if (++txShown <= 128 || view.header.operation == 1 || view.header.operation == 264 ||
        view.header.operation == 4 || view.header.operation == 5 || view.header.operation == 8 ||
        view.header.operation == 265 || view.header.operation == 267) {
        printf("UKE WIFI CTL: send session %llu unit %u bytes %zu kind %u op %u seq %u status %d socket %d decode %d\n",
            session, unit, length, view.header.kind, view.header.operation, view.header.sequence,
            view.header.status, result, decoded);
    }
    // Full wire header for EVENT/REPLY classification. The userspace client
    // filters on session and decodes header.length == recvmsg length; these
    // fields are what it will see. Needed to tell "never read" from
    // "read but rejected" after a JOIN_REQUEST is published.
    if (view.header.kind == 3 || view.header.operation == 1) {
        printf("UKE WIFI CTL: HDR op %u kind %u seq %u len %u status %d session %llu gen %llu\n",
               view.header.operation, view.header.kind, view.header.sequence,
               view.header.length, view.header.status, view.header.session, view.header.generation);
    }
    if (!result) return NW_CONTROL_SEND_OK;
    if (result == ENOBUFS || result == EWOULDBLOCK) return NW_CONTROL_SEND_AGAIN;
    return NW_CONTROL_SEND_CLOSED;
}
void controlWake(void *) {uke_wifi_ahb_wake();}
int controlStart(void) {
    if (gControlRef) return 0;
    if (!gControlLock) gControlLock = IOLockAlloc();
    if (!gControlLock) return ENOMEM;
    kern_ctl_reg registration{};
    strlcpy(registration.ctl_name, NW_CONTROL_NAME, sizeof registration.ctl_name);
    registration.ctl_flags = CTL_FLAG_PRIVILEGED | CTL_FLAG_REG_EXTENDED;
    registration.ctl_sendsize = 256 * 1024; registration.ctl_recvsize = 512 * 1024;
    registration.ctl_connect = controlConnect; registration.ctl_disconnect = controlDisconnect;
    registration.ctl_send = controlReceive; registration.ctl_getopt = controlGetOption; registration.ctl_rcvd = controlReceived;
    const errno_t result = ctl_register(&registration, &gControlRef);
    if (result) {printf("UKE WIFI AHB: ctl_register failed %d\n", result); return result;}
    const struct nw_control_transport transport = {nullptr, controlSend, controlWake};
    const int status = nw_wifi_control_transport_set(&transport);
    if (status) {ctl_deregister(gControlRef); gControlRef = nullptr; return controlError(status);}
    return 0;
}
int controlSelfTestRead(void *context, void *destination, size_t length) {
    memcpy(destination, context, length); return 0;
}
// One NW_CONTROL_INFO request through the real queue, then detach. Runs from the
// WPSS worker: nw_wifi_step() polls the control queue, so the reply comes back
// through controlSend on a later iteration.
void controlSelfTestStep(void) {
    static int state;
    if (state == 2 || !nw_cfg80211_primary()) return;
    if (state == 0) {
        uint8_t message[64];
        nw_control_builder builder;
        uint64_t session = 0;
        int status = nw_wifi_control_attach(&session);
        if (status) {printf("UKE WIFI AHB: control selftest attach %d\n", status); state = 2; return;}
        if (nw_control_begin(&builder, message, sizeof message, NW_CONTROL_REQUEST, NW_CONTROL_INFO, 7, session, 0, 0)) {
            printf("UKE WIFI AHB: control selftest encode failed\n");
            nw_wifi_control_detach(session); state = 2; return;
        }
        gSelfTestSession = session;
        status = nw_wifi_control_receive(session, builder.length, controlSelfTestRead, message);
        printf("UKE WIFI AHB: control selftest request %d session %llu len %zu\n", status, session, (size_t)builder.length);
        state = 1; return;
    }
    if (!gSelfTestReply) return;
    nw_control_stats stats;
    nw_wifi_control_stats_get(&stats);
    printf("UKE WIFI AHB: control selftest done accepted %llu dispatched %llu replies %llu\n",
           stats.accepted, stats.dispatched, stats.replies);
    nw_wifi_control_detach(gSelfTestSession);
    gSelfTestSession = 0;
    state = 2;
}

// ---- services the ported network layer calls (package/WifiNet.cpp) ----------
// sheng's WifiNet logs through native_wifi_logf, reads join.txt through
// sheng_wifi_file (autoJoin, only under the sheng-wifi-aj boot argument) and
// wakes the worker with sheng_wifi_wake. The interface itself is created by
// sheng_wifi_net_start, which WifiNet.cpp provides.
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_wifi_net_static_init(void) __asm__("__GLOBAL__sub_I_WifiNet.cpp");
extern "C" void sheng_wifi_net_start(const uint8_t mac[6]);

extern "C" void native_wifi_logf(int level, const char *fmt, ...) {
    char line[160];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    (void)level;
    printf("UKE WIFI NET: %s\n", line);
}
// This device has no join.txt source (it is not on the modem FAT and the board
// has no VFS reader for Data here), so autoJoin logs once and stops; the
// settings/wifid path is the one that joins.
extern "C" int sheng_wifi_file(const char *name, const uint8_t **data, size_t *size) {
    (void)data; (void)size;
    printf("UKE WIFI NET: no file service for %s\n", name ? name : "(null)");
    return -ENOENT;
}
extern "C" void sheng_wifi_wake(void) {uke_wifi_ahb_wake();}

// WifiNet's classes are registered late like the other owned classes: run its
// static constructors inside the module window, then create en0 from the
// station's MAC once the netdev is ready (stage 4 carries the address).
void networkStartStep(void) {
    static int state;
    if (state || nw_wifi_stats.stage < 4) return;
    struct nw_wifi_status status;
    nw_wifi_status_get(&status);
    void *handle = ukeClassBegin(g_kernel_kmod_info.name);
    if (!handle) {printf("UKE WIFI NET: no module handle\n"); state = 2; return;}
    native_wifi_net_static_init();
    const OSReturn registered = ukeClassEnd(handle);
    if (registered) {printf("UKE WIFI NET: classes not registered: %d\n", (int)registered); state = 2; return;}
    sheng_wifi_net_start(status.mac);
    printf("UKE WIFI NET: en0 started with %02x:%02x:%02x:%02x:%02x:%02x\n",
           status.mac[0], status.mac[1], status.mac[2], status.mac[3], status.mac[4], status.mac[5]);
    state = 1;
}


// WMI command, every HTC service/credit event and every mac80211 callback
// through ath11k_dbg(), so no board-side trace is needed to see the command
// sequence. Bits are the upstream enum (debug.h): WMI 0x2, HTC 0x4, MAC 0x10,
// BOOT 0x20, QMI 0x40. The log callback filters high-volume debug as in sheng;
// unfiltered WMI/HTC output can itself stall control RPCs. Override with the boot argument
// ath11k-debug-mask=<n>, 0 to silence.
constexpr unsigned kAth11kDebugMask = 0x76;
unsigned ath11kDebugMask() {
    unsigned mask = kAth11kDebugMask;
    PE_parse_boot_argn("ath11k-debug-mask", &mask, sizeof mask);
    return mask;
}

extern "C" int uke_wifi_ath11k_start(void) {
    ath11k_debug_mask = ath11kDebugMask();
    printf("UKE WIFI AHB: ath11k debug mask %08x\n", ath11k_debug_mask);
    if (!windowInit()) {
        printf("UKE WIFI AHB: ShengUfs uncached endpoint window missing; ath11k not started\n");
        return -1;
    }
    if (!msiSpiAttach()) {
        IOFree(gArena.blocks, kDmaBlocks * sizeof(Arena::Block));
        gArena = {};
        return -ENXIO;
    }
    // The board half of the ABI (nw_abi.h): the register window from the QMI
    // device-info reply, this board's IRQ numbering for the MSI bitmap, and the
    // persistent mapping used for MMIO. cfg_read/cfg_write/msi_target stay null:
    // the AHB endpoint has no configuration space and writes its own doorbell.
    static struct nw_board board;
    board.bar = nw_board_bar();
    board.bar_len = kWlanBarBytes;
    board.irq_base = 0x400;
    board.map_phys = nw_board_map_phys;
    // The transport has to exist before cfg80211's nl80211_init binds its
    // dispatch; both happen inside nw_wifi_start(), which also publishes the
    // device class and root device wiphy_register's device_add() needs. The
    // platform goes in first: the control queue's locks go through it.
    nw_wifi_set_platform(&gPlatform);
    const int control = controlStart();
    printf("UKE WIFI AHB: control endpoint %d\n", control);
    if (control) return -5;
    nw_wifi_irq_mode(1);   // hardware MSI, not the polling fallback
    const int started = nw_wifi_start(&gPlatform, &board);
    const void *primary = nw_cfg80211_primary();
    printf("UKE WIFI AHB: ath11k start %d stage %u err %d primary %s\n", started,
           nw_wifi_stats.stage, nw_wifi_stats.last_err, primary ? "present" : "absent");
    return started;
}

// Called from the WPSS worker next to lx_pump: one step of the module's own
// lifecycle (control poll, dispatcher, station run) plus the bring-up probes.
extern "C" void uke_wifi_station_step(void) {
    const uint64_t began = nowMs();
    nw_wifi_step();
    const uint64_t elapsed = nowMs() - began;
    if (elapsed > gStepMaxMs) gStepMaxMs = elapsed;
    // The one-shot self-test session is retired: it held the single control
    // session, and the real supplicant needs it.
    networkStartStep();
    static uint64_t next, nextCtl;
    static int periodic = -1;   // the five-second ring and control rows: log-wifi=1
    if (periodic < 0) {uint32_t v = 0; PE_parse_boot_argn("log-wifi", &v, sizeof v); periodic = v != 0;}
    if (!periodic) return;
    const uint64_t now = nowMs();
    if (nw_cfg80211_primary() && now >= next) {
        next = now + 5000;
        char rings[256];
        nw_wifi_ring_state(rings, sizeof rings);
        printf("UKE WIFI AHB: rings %s\n", rings);
    }
    // Unbounded per-operation control counters. These survive any per-session
    // print bound, so the join window (255-289s in V4.8) is always observable
    // even when the RX/TX trace lines have stopped. Ops: 1 INFO 2 SCAN
    // 3 GET_BSS 4 AUTH 5 ASSOC 7 SET_KEY 8 SET_PORT 257 SCAN_STARTED
    // 258 SCAN_DONE 264 JOIN_REQUEST 265 DISCONNECT 267 SCAN_REQUEST
    // 270 WIRELESS_EVENT.
    if (now >= nextCtl) {
        nextCtl = now + 5000;
        printf("UKE WIFI CTLWORK: v493 polls %llu gapmax %llu ms stepmax %llu ms logdrop %u\n",
               gPollCalls, gPollGapMaxMs, gStepMaxMs, gLogSuppressed);
        printf("UKE WIFI CTLSTAT: rx %u tx %u rcvd %u rxlast %u txlast %u "
               "rx[1 %u 2 %u 3 %u 4 %u 5 %u 7 %u 8 %u 257 %u 258 %u 264 %u 265 %u 267 %u 270 %u] "
               "tx[1 %u 2 %u 3 %u 4 %u 5 %u 7 %u 8 %u 257 %u 258 %u 264 %u 265 %u 267 %u 270 %u]\n",
               gCtlRxTotal, gCtlTxTotal, gCtlRcvdTotal,
               gCtlRxLastStatus, gCtlTxLastSocket,
               gCtlRxOp[0], gCtlRxOp[1], gCtlRxOp[2], gCtlRxOp[3], gCtlRxOp[4], gCtlRxOp[5],
               gCtlRxOp[6], gCtlRxOp[7], gCtlRxOp[8], gCtlRxOp[9], gCtlRxOp[10], gCtlRxOp[11], gCtlRxOp[12],
               gCtlTxOp[0], gCtlTxOp[1], gCtlTxOp[2], gCtlTxOp[3], gCtlTxOp[4], gCtlTxOp[5],
               gCtlTxOp[6], gCtlTxOp[7], gCtlTxOp[8], gCtlTxOp[9], gCtlTxOp[10], gCtlTxOp[11], gCtlTxOp[12]);
    }
}

// Bring-up verification: once the default interface exists, ask cfg80211 for one
// full scan. This is the same path a userspace request takes (cfg80211_scan ->
// rdev_scan -> ath11k_mac_op_hw_scan -> WMI), so a failure here is a real one.
static bool gScanRequested;
extern "C" void uke_wifi_scan_selftest(void) {
    if (gScanRequested || !nw_cfg80211_primary()) return;
    gScanRequested = true;
    char rings[256];
    nw_wifi_ring_state(rings, sizeof rings);
    printf("UKE WIFI AHB: rings before scan %s\n", rings);
    const int result = nw_cfg80211_selftest_scan();
    printf("UKE WIFI AHB: selftest scan %d\n", result);
}
