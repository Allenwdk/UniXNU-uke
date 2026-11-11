// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native sheng SM8550 UFS bring-up. Default-off: no MMIO is touched unless
// native-ufs-probe=1 is present. This is a register probe, not a storage driver.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <libkern/crypto/sha2.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
/* Newer XNU keeps the kernel memory and map protection interfaces in these headers. */
#if __has_include(<vm/vm_kern_xnu.h>)
#include <vm/vm_kern_xnu.h>
#endif
#include <mach/mach_vm.h>
#include <sys/param.h>
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
#include "verified_root.h"
#include "VerifiedBlockPolicy.h"
extern "C" {
extern const struct vr_crypto vr_xnu_crypto;
extern const uint8_t verified_root_key[];
}
#endif
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>
#include <IOKit/IOBSD.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
extern "C" {
#include <kern/ast.h>   // ast_pending: no C linkage block of its own
}
#include <machine/machine_routines.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include "GccUfs.h"
#include "RootDiagnostics.h"
#include "TransferPoll.h"
#include "CompletionWait.h"
#include "SmmuTakeover.h"

static UfsRootDiagnostics gRootDiagnostic;
static bool gRootDiagnosticEnabled;
static void rootDiagnosticFailure (const char *reason);

extern "C" {
// Optional passive observer; absent in ordinary builds. It may run while a
// window lock is held, so its contract forbids blocking or transport access.
#if defined(NATIVE_UFS_SOURCE_API) && NATIVE_UFS_SOURCE_API
// The source runtime archive carries a weak no-op definition; a weak
// reference here would become an unresolved pointer-auth import instead.
void boot_progress_poll (void);
#else
void boot_progress_poll (void) __attribute__((weak));
#endif
volatile uint32_t native_ufs_probe_calls = 0;
volatile uint32_t native_ufs_matched = 0;
volatile uint32_t native_ufs_probe_requested = 0;
volatile uint32_t native_ufs_mapped = 0;
volatile uint32_t native_ufs_status = 0;
// Root start: 1 entered, 2 host up, 3 layout found; the last soft-failure reason (a static string), for the boot page.
volatile uint32_t native_ufs_root_step = 0;
const char *volatile native_ufs_root_failure = nullptr;
volatile uint32_t native_ufs_cap = 0;
volatile uint32_t native_ufs_version = 0;
volatile uint32_t native_ufs_hcs = 0;
volatile uint32_t native_ufs_hce = 0;
volatile uint32_t native_ufs_utrl_ready = 0;
volatile uint32_t native_ufs_uic_status = 0;
volatile uint32_t native_ufs_uic_result = 0;
}

class NativeUfs : public IOService {
  OSDeclareDefaultStructors (NativeUfs)
  OSSharedPtr<IOMemoryMap> fMap;
  OSSharedPtr<IOBufferMemoryDescriptor> fCmdDesc;
  void inspect (void);
  void adoptAndRead (unsigned level);
  void dumpFirmwareList (void);
  void inspectSmmu (void);
  void inspectContextBanks (void);
  bool startRoot (IOService *provider);
  static bool waitSet (uintptr_t base, unsigned offset, uint32_t mask, unsigned loops);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void stop (IOService *provider) override;
};

OSDefineMetaClassAndStructors (NativeUfs, IOService)

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (base + offset) = value;
}

static bool contains (const char *data, size_t length, const char *needle)
{
  size_t wanted = 0;
  while (needle[wanted]) {
    wanted++;
  }
  for (size_t at = 0; at + wanted <= length; at++) {
    size_t index = 0;
    while (index < wanted && data[at + index] == needle[index]) {
      index++;
    }
    if (index == wanted) {
      return true;
    }
  }
  return false;
}

IOService *NativeUfs::probe (IOService *provider, SInt32 *score)
{
  native_ufs_probe_calls = native_ufs_probe_calls + 1;
  if (OSDynamicCast (IOPlatformExpertDevice, provider)) {
    // Second personality (KernelConfigTables: IOMatchCategory NativeUfsRoot): attaches to the
    // root device, which starts before the platform expert publishes IOResources, so the
    // hardware probes run before the original AMFI (whose start panics without a PMGR).
    native_ufs_matched = 1;
    if (score) {
      *score = 500;
    }
    return this;
  }
  OSObject *property = provider->getProperty ("compatible");
  OSData *data = OSDynamicCast (OSData, property);
  OSString *string = OSDynamicCast (OSString, property);
  bool match = false;
  if (data) {
    const char *text = static_cast<const char *> (data->getBytesNoCopy ());
    match = text && contains (text, data->getLength (), "qcom,sm8550-ufshc");
  } else if (string) {
    const char *text = string->getCStringNoCopy ();
    match = text && contains (text, string->getLength (), "qcom,sm8550-ufshc");
  }
  if (!match) {
    return nullptr;
  }
  native_ufs_matched = 1;
  if (score) {
    *score = 1000;
  }
  return this;
}

bool NativeUfs::waitSet (uintptr_t base, unsigned offset, uint32_t mask, unsigned loops)
{
  for (unsigned index = 0; index < loops; index++) {
    if ((read32 (base, offset) & mask) == mask) {
      return true;
    }
    IODelay (1);
  }
  return false;
}

// Read-only inspection (native-ufs-probe=2): reports what firmware left behind.
// The physical windows are the sheng board DT's own GCC (0x100000) and UFS host
// (0x1d84000, 0x3000) ranges. The controller is read only if the GCC says its
// GDSC is powered and the AXI/AHB branches are running; nothing is written.
static uintptr_t mapPhys (uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, length);
  if (!memory) {
    return 0;
  }
  keep = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release ();
  return keep ? static_cast<uintptr_t> (keep->getVirtualAddress ()) : 0;
}

void NativeUfs::inspect (void)
{
  OSSharedPtr<IOMemoryMap> gccMap, memPhyMap, hcMap;
  uintptr_t gcc = mapPhys (GCC_BASE + 0x77000, 0x1000, gccMap);
  uintptr_t memPhy = mapPhys (GCC_BASE + 0x9e000, 0x1000, memPhyMap);
  if (!gcc || !memPhy) {
    panic ("NATIVE_UFS inspect: GCC map failed gcc=%p memphy=%p", (void *) gcc, (void *) memPhy);
  }
  uint32_t gdsc = read32 (gcc, GCC_UFS_PHY_GDSCR - 0x77000);
  uint32_t axi = read32 (gcc, GCC_UFS_PHY_AXI_ENABLE - 0x77000);
  uint32_t ahb = read32 (gcc, GCC_UFS_PHY_AHB_ENABLE - 0x77000);
  uint32_t unipro = read32 (gcc, GCC_UFS_PHY_UNIPRO_CORE_ENABLE - 0x77000);
  uint32_t txs = read32 (gcc, GCC_UFS_PHY_TX_SYMBOL0_ENABLE - 0x77000);
  uint32_t rxs = read32 (gcc, GCC_UFS_PHY_RX_SYMBOL0_ENABLE - 0x77000);
  uint32_t aux = read32 (gcc, GCC_UFS_PHY_PHY_AUX_ENABLE - 0x77000);
  uint32_t memGdsc = read32 (memPhy, 0);
  // GDSCR bit31 PWR_ON; CBCR bit31 CLK_OFF.
  bool powered = (gdsc & 0x80000000U) != 0;
  // Hardware 2026-10-17: AXI running, AHB enabled but CLK_OFF (bit31). With bit1 (HW gating)
  // set the branch wakes on access, so enable bits, not CLK_OFF, gate the read-only probe.
  bool busClocks = !(axi & 0x80000000U) && (ahb & 0x1U) && (axi & 0x1U);
  uint32_t cap = 0, ver = 0, hcs = 0, hce = 0, utrlba = 0, is = 0;
  unsigned readHc = 0;
  if (powered && busClocks) {
    uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
    if (hc) {
      cap = read32 (hc, 0x00);
      ver = read32 (hc, 0x08);
      is = read32 (hc, 0x20);
      hcs = read32 (hc, 0x30);
      hce = read32 (hc, 0x34);
      utrlba = read32 (hc, 0x50);
      readHc = 1;
    }
  }
  panic ("NATIVE_UFS inspect\n"
         "gdsc=%08x memphygdsc=%08x\n"
         "axi=%08x ahb=%08x unipro=%08x\n"
         "txsym=%08x rxsym=%08x aux=%08x\n"
         "hc_read=%u cap=%08x ver=%08x\n"
         "is=%08x hcs=%08x hce=%08x utrlba=%08x",
         gdsc, memGdsc, axi, ahb, unipro, txs, rxs, aux, readHc, cap, ver, is, hcs, hce, utrlba);
}

// Adopt the controller the firmware left running (native-ufs-probe=3) and issue
// read-only SCSI commands through one UTP transfer request slot. Nothing is written
// to the storage. The controller is not reset and the link is not restarted; the
// firmware's request list is stopped (refused if a request is outstanding) and replaced
// by ours. Interrupts stay masked and completion is polled.
namespace {
struct UfsResult {
  uint32_t ocs;      // UTRD overall command status; 0xf = never completed
  uint32_t response; // UPIU response byte
  uint32_t scsi;     // SCSI status byte
  uint32_t sense;    // sense key<<16 | ASC<<8 | ASCQ from the response UPIU's autosense data
  uint32_t senseLen; // sense data length reported by the device
  uint32_t seg;      // UPIU data segment length
};

static void be32 (uint8_t *at, uint32_t value)
{
  at[0] = value >> 24; at[1] = value >> 16; at[2] = value >> 8; at[3] = value;
}

static uint32_t getBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}
}

static void cacheClean (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc cvac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void cacheCleanInvalidate (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// The region is mapped as device memory: only aligned element-sized stores are allowed
// (bzero's block zeroing faulted on hardware, 2026-10-17).
static void zero32 (void *start, size_t length)
{
  volatile uint32_t *word = static_cast<volatile uint32_t *> (start);
  for (size_t index = 0; index < length / 4; index++) {
    word[index] = 0;
  }
}


// Give the UFS stream a pass-through mapping the way Linux does on this firmware.
// Hardware 2026-10-17: after hand-off apps_smmu has sCR0 = 0x002f0406 (USFCFG=1, unmatched
// streams fault), no stream-match entry for the UFS host (ID 0x60, `iommus = <&apps_smmu 0x60
// 0x0>` in the board DT), a TYPE=BYPASS S2CR reads back as FAULT, and sCR0 writes are ignored.
// Linux (drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c, qcom_smmu_cfg_probe + qcom_smmu_write_s2cr,
// "bypass_quirk") meets exactly this: it reserves the LAST context bank (num_context_banks-1),
// writes SCTLR=0 (stage 1 disabled) and CBAR.TYPE=S1_TRANS_S2_BYPASS there, and maps a bypass
// stream as S2CR TYPE=TRANSLATE with CBNDX = that bank. S2CR is written before SMR
// (arm_smmu_write_sme). ARM IHI 0062D.c table 3-1 additionally requires VA64=1
// for inputs above 32 bits even with SCTLR.M=0. Storage DMA path only; this
// is physical pass-through, not a page-table or Adreno 4 KiB qualification.
extern "C" volatile uint32_t native_ufs_extra_stream;
extern "C" uint8_t *native_ufs_extra_va;
extern "C" volatile uint32_t native_ufs_extra_uncached;
extern "C" volatile uint32_t native_ufs_extra_iova, native_ufs_extra_bytes;
extern "C" volatile uint32_t native_ufs_msi_iova;   // MSI doorbell reached through this bank (see below)
extern "C" volatile uint32_t native_ufs_extra_atos[8];
extern "C" volatile uint32_t native_ufs_dpu_smmu[24];
extern "C" volatile uint32_t native_ufs_dpu_state;
extern "C" volatile uint32_t native_ufs_dpu_check[8];
// The display bank's stage 1 table: L1, then L2 for the first GiB, then kDpuSlots * kDpuSlotTables L3 tables (one per 2 MiB of a slot).
constexpr uint32_t kDpuWindow = 0x10000000U, kDpuSlots = 4, kDpuSlotBytes = 32U << 20, kDpuSlotTables = kDpuSlotBytes >> 21;
static uint64_t *gDpuTables;
static uintptr_t gDpuCb;
static IOLock *gDpuLock;
static uintptr_t gDpuLiveCb;
// FSR fault bits (TF .. UUT, SS, MULTI); 0x600 is the format field.
constexpr uint32_t kDpuFaults = 0xc00001feU;
static void dpuClean (const void *start, size_t length)
{
  for (size_t off = 0; off < length; off += 64)
    __asm__ volatile ("dc civac, %0" :: "r" (static_cast<const uint8_t *> (start) + off) : "memory");
  __asm__ volatile ("dsb sy" ::: "memory");
}
static bool dpuTlbFlush (uintptr_t cb)
{
  write32 (cb, 0x618, 0); write32 (cb, 0x7f0, 0);   // TLBIALL, TLBSYNC
  __asm__ volatile ("dsb sy" ::: "memory");
  for (unsigned spin = 0; spin < 100000; ++spin) if (!(read32 (cb, 0x7f4) & 1)) return true;
  return false;
}
static void dpuEnable (uintptr_t smmu, uintptr_t cb, uint32_t bank, uint32_t oldSctlr)
{
  (void) smmu;
  const size_t bytes = (2 + kDpuSlots * kDpuSlotTables) * 4096;
  IOBufferMemoryDescriptor *tables = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone), bytes, 4096);
  gDpuLock = IOLockAlloc ();
  if (!tables || !gDpuLock) {native_ufs_dpu_state = 0xdead0002U; return;}
  uint64_t *l1 = static_cast<uint64_t *> (tables->getBytesNoCopy ()), *l2 = l1 + 512;
  const uint64_t pa = tables->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
  bzero (l1, bytes);
  // Normal non-cacheable (attribute 0 = 0x44), not shareable, access flag, readable and writable at both privilege levels.
  const uint64_t attrs = (1ULL << 10) | (1ULL << 6);
  // i564: the first fetch faulted at once with FSYNR0 level 0, so the walk may start at level 0 although T0SZ 32 means level 1. One table
  // serves every level: [0] points back at itself (level 0 -> level 1, level 1 -> level 2 for the window), [2] and [3] are the 1 GiB
  // blocks, [128 + t] the window's level 3 tables. The second page stays unused.
  (void) l2;
  l1[0] = pa | 3;
  l1[2] = 0x80000000ULL | attrs | 1;
  l1[3] = 0xc0000000ULL | attrs | 1;
  for (uint32_t t = 0; t < kDpuSlots * kDpuSlotTables; ++t) l1[(kDpuWindow >> 21) + t] = (pa + (2 + t) * 4096ULL) | 3;
  dpuClean (l1, bytes);
  const uint32_t old[5] = {read32 (cb, 0x20), read32 (cb, 0x24), read32 (cb, 0x10), read32 (cb, 0x30), read32 (cb, 0x38)};
  gDpuLiveCb = cb;
  write32 (cb, 0x20, static_cast<uint32_t> (pa)); write32 (cb, 0x24, static_cast<uint32_t> (pa >> 32));
  // TCR2 (PASIZE bits 2:0, SEP bits 17:15): 48-bit output and SEP 7 (the sign extension comes from the upstream address, as arm-smmu's AArch64 contexts set it). SEP 0 takes
  // IA[31] as the sign bit, so every address with bit 31 set but zero above it (the display's own framebuffer at 0xb90f7500: i564, and the
  // first fetch of every earlier try) is out of TTBR0's range and faults at level 0.
  write32 (cb, 0x10, 5U | (7U << 15));
  write32 (cb, 0x30, 32U | (1U << 23));               // T0SZ 32, non-cacheable walks, 4 KiB granule, no TTBR1
  write32 (cb, 0x38, 0x44);                           // MAIR attribute 0: normal non-cacheable
  __asm__ volatile ("dsb sy" ::: "memory");
  volatile uint32_t *c = native_ufs_dpu_check;
  c[0] = read32 (cb, 0x20); c[1] = read32 (cb, 0x30); c[2] = read32 (cb, 0x58);
  const auto undo = [&] (uint32_t why) {
    write32 (cb, 0x0, oldSctlr);
    __asm__ volatile ("dsb sy; isb" ::: "memory");
    write32 (cb, 0x20, old[0]); write32 (cb, 0x24, old[1]); write32 (cb, 0x10, old[2]); write32 (cb, 0x30, old[3]); write32 (cb, 0x38, old[4]);
    const uint32_t fsr = read32 (cb, 0x58);
    if (fsr & (1U << 30)) write32 (cb, 0x8, 1);        // RESUME: terminate a stalled transaction
    if (fsr & kDpuFaults) write32 (cb, 0x58, fsr);     // write one to clear
    dpuTlbFlush (cb);
    native_ufs_dpu_state = why;
  };
  if (c[0] != static_cast<uint32_t> (pa) || c[1] != (32U | (1U << 23))) {undo (0xdead0003U); return;}   // the bank ignores our writes
  if (!dpuTlbFlush (cb)) {undo (0xdead0004U); return;}
  write32 (cb, 0x0, 1U | (1U << 5) | (1U << 6));      // SCTLR: M, CFRE, CFIE; a fault terminates instead of stalling the display
  __asm__ volatile ("dsb sy; isb" ::: "memory");
  // No check translation here (i563: IDR0 bit 26 ATOSNS, the SMMU has no ATOS), so watch the display's own fetches for 50 ms (6 frames
  // at 120 Hz) and turn translation off again at the first fault.
  uint32_t fsr = 0, us = 0;
  for (; us < 50000; us += 500) {
    fsr = read32 (cb, 0x58);
    if (fsr & kDpuFaults) break;
    IODelay (500);
  }
  c[3] = fsr; c[7] = us;
  if (fsr & kDpuFaults) {
    c[4] = read32 (cb, 0x60); c[5] = read32 (cb, 0x64); c[6] = read32 (cb, 0x68);
    undo (0xdead0005U); return;
  }
  gDpuTables = l1; gDpuCb = cb;
  native_ufs_dpu_state = 0x80000000U | bank;
}
static uintptr_t gExtraSmmu, gExtraCb;
static unsigned gExtraBank;

static void smmuBypassStream (uint16_t stream, OSSharedPtr<IOMemoryMap> &keep, uint32_t *report,
                              uint64_t dma, uint64_t bytes)
{
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, keep);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  UfsSmmuPlan plan = planUfsSmmu (stream, dma, bytes, [smmu](unsigned offset) { return read32 (smmu, offset); });
  gRootDiagnostic.smmuId1 = read32 (smmu, 0x24);
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.smmuSlot = plan.slot;
  if (plan.error) {
    gRootDiagnostic.stage = "SMMU planning";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    gRootDiagnostic.smr = plan.smr; gRootDiagnostic.s2cr = plan.s2cr;
    rootDiagnosticFailure (plan.error);
    panic ("NATIVE_UFS smmu: %s slot=%u smr=%x s2cr=%x", plan.error, plan.slot, plan.smr, plan.s2cr);
  }
  unsigned bank = plan.bank, index = plan.slot;
  uintptr_t cb = smmu + (plan.pages + bank) * 4096;
  const char *failure = configureUfsSmmu (plan, stream,
    [smmu](unsigned offset) { return read32 (smmu, offset); },
    [smmu](unsigned offset, uint32_t value) {
      write32 (smmu, offset, value);
      __asm__ volatile ("dsb sy" ::: "memory");
    });
  uint32_t smr = read32 (smmu, 0x800 + 4 * index), s2cr = read32 (smmu, 0xc00 + 4 * index);
  uint32_t cbar = read32 (smmu, 0x1000 + 4 * bank), sctlr = read32 (cb, 0x0);
  gRootDiagnostic.smr = smr; gRootDiagnostic.s2cr = s2cr;
  gRootDiagnostic.cbar = cbar; gRootDiagnostic.sctlr = sctlr;
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.cba2r = read32 (smmu, 0x1800 + 4 * bank);
  if (failure) {
    gRootDiagnostic.stage = "SMMU readback";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    rootDiagnosticFailure (failure);
    panic ("NATIVE_UFS smmu: %s idx=%u bank=%u cba2r=%x", failure, index, bank,
           gRootDiagnostic.cba2r);
  }
  report[0] = index | (bank << 16);
  report[1] = cbar;
  report[2] = sctlr;
}

void NativeUfs::adoptAndRead (unsigned level)
{
  OSSharedPtr<IOMemoryMap> hcMap;
  uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
  if (!hc) {
    panic ("NATIVE_UFS adopt: controller map failed");
  }
  // The controller's own request list is the one region its DMA is known to reach
  // (hardware 2026-10-17: a kernel-allocated buffer at 0xf080f000 reset the system when the
  // first command was issued, and nothing below 0xc8000000 is allocatable). The firmware
  // zeroed it; use 16 KiB from UTRLBA as ordinary cached memory (the DT marks the host
  // dma-coherent). Test instrument only: the kernel may also own these pages.
  uint64_t fwList = (uint64_t (read32 (hc, 0x54)) << 32) | read32 (hc, 0x50);
  if (!fwList || (fwList & 0xfff)) {
    panic ("NATIVE_UFS adopt: no usable firmware list base=%llx", fwList);
  }
  IOMemoryDescriptor *region = IOMemoryDescriptor::withPhysicalAddress (fwList, 16384, kIODirectionInOut);
  OSSharedPtr<IOMemoryMap> regionMap;
  if (region) {
    regionMap = OSSharedPtr<IOMemoryMap> (region->map (kIOMapAnywhere), OSNoRetain);
    region->release ();
  }
  if (!regionMap) {
    panic ("NATIVE_UFS adopt: firmware list map failed base=%llx", fwList);
  }
  uint8_t *virt = reinterpret_cast<uint8_t *> (regionMap->getVirtualAddress ());
  uint64_t phys = fwList;
  zero32 (virt, 16384);
  cacheClean (virt, 16384);
  // Layout: [0,4096) request list (slot 0 used), [4096,5120) command + response UPIU +
  // PRDT, [8192,16384) data.
  const uint32_t ucdOffset = 4096, rspOffset = 512, prdtOffset = 1024, dataOffset = 8192;
  uint32_t hcsBefore = read32 (hc, 0x30);
  uint32_t doorbellBefore = read32 (hc, 0x58);
  if (doorbellBefore != 0 || read32 (hc, 0x78) != 0) {
    panic ("NATIVE_UFS adopt: firmware request outstanding doorbell=%08x hcs=%08x", doorbellBefore, hcsBefore);
  }
  OSSharedPtr<IOMemoryMap> smmuMap;
  uint32_t smmuReport[3] = { 0, 0, 0 };
  write32 (hc, 0x24, 0);            // mask interrupts
  write32 (hc, 0x60, 0);            // stop the firmware's request list
  __asm__ volatile ("dsb sy" ::: "memory");
  unsigned stopLoops = 10000;
  while ((read32 (hc, 0x60) & 1) && stopLoops--) IODelay (10);
  if (read32 (hc, 0x60) & 1) panic ("NATIVE_UFS adopt: request list did not stop");
  smmuBypassStream (0x60, smmuMap, smmuReport, phys, 16384);
  write32 (hc, 0x50, static_cast<uint32_t> (phys));
  write32 (hc, 0x54, static_cast<uint32_t> (phys >> 32));
  write32 (hc, 0x20, 0xffffffffU);  // clear stale status
  write32 (hc, 0x60, 1);
  if (!waitSet (hc, 0x30, 0x2, 100000)) {
    panic ("NATIVE_UFS adopt: request list not ready hcs=%08x", read32 (hc, 0x30));
  }
  if (level == 3) {
    panic ("NATIVE_UFS adopt: list replaced\n"
           "phys=%llx hcs=%08x->%08x\n"
           "is=%08x db=%08x rsr=%08x ba=%08x bau=%08x ie=%08x",
           phys, hcsBefore, read32 (hc, 0x30), read32 (hc, 0x20), read32 (hc, 0x58),
           read32 (hc, 0x60), read32 (hc, 0x50), read32 (hc, 0x54), read32 (hc, 0x24));
  }

  auto command = [&] (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t length) -> UfsResult {
    uint8_t *utrd = virt;
    uint8_t *ucd = virt + ucdOffset;
    zero32 (utrd, 32);
    zero32 (ucd, 1024 + 16);
    zero32 (virt + dataOffset, 8192);
    ucd[0] = 0x01;                  // COMMAND UPIU
    ucd[1] = toHost ? 0x40 : 0x00;  // flags: read
    ucd[2] = lun;
    ucd[3] = 0;                     // task tag
    be32 (ucd + 12, length);        // expected data transfer length
    for (unsigned index = 0; index < cdbLength; index++) {
      ucd[16 + index] = cdb[index];
    }
    uint64_t ucdPhys = phys + ucdOffset;
    uint32_t direction = length ? (toHost ? 2U : 1U) : 0U;
    *reinterpret_cast<uint32_t *> (utrd + 0) = 0x10000000U | (direction << 25);
    *reinterpret_cast<uint32_t *> (utrd + 8) = 0x0fU;     // OCS: not completed
    *reinterpret_cast<uint32_t *> (utrd + 16) = static_cast<uint32_t> (ucdPhys);
    *reinterpret_cast<uint32_t *> (utrd + 20) = static_cast<uint32_t> (ucdPhys >> 32);
    *reinterpret_cast<uint32_t *> (utrd + 24) = (512U >> 2) | ((rspOffset >> 2) << 16);   // response length | offset (dwords)
    uint32_t prdEntries = length ? 1U : 0U;
    *reinterpret_cast<uint32_t *> (utrd + 28) = prdEntries | ((prdtOffset >> 2) << 16);
    if (length) {
      uint64_t dataPhys = phys + dataOffset;
      uint8_t *prd = ucd + prdtOffset;
      *reinterpret_cast<uint32_t *> (prd + 0) = static_cast<uint32_t> (dataPhys);
      *reinterpret_cast<uint32_t *> (prd + 4) = static_cast<uint32_t> (dataPhys >> 32);
      *reinterpret_cast<uint32_t *> (prd + 12) = length - 1;
    }
    cacheClean (virt, 16384);
    write32 (hc, 0x20, 0xffffffffU);
    write32 (hc, 0x58, 1);          // ring slot 0
    UfsResult result = { 0xf, 0xff, 0xff, 0, 0, 0 };
    bool done = false;
    for (unsigned loops = 0; loops < 100000; loops++) {
      if (!(read32 (hc, 0x58) & 1)) {
        done = true;
        break;
      }
      IODelay (1);
    }
    cacheCleanInvalidate (virt, 16384);
    if (!done) {
      return result;
    }
    result.ocs = *reinterpret_cast<volatile uint32_t *> (utrd + 8) & 0xff;
    const uint8_t *rsp = ucd + rspOffset;
    result.response = rsp[6];
    result.scsi = rsp[7];
    // Response UPIU: 12-byte header, residual count, 16 reserved bytes, then the data segment
    // (2-byte sense length + sense data) at offset 32.
    result.seg = (uint32_t (rsp[10]) << 8) | rsp[11];
    result.senseLen = (uint32_t (rsp[32]) << 8) | rsp[33];
    result.sense = ((rsp[36] & 0xfU) << 16) | (uint32_t (rsp[46]) << 8) | rsp[47];
    return result;
  };

  // NOP OUT (kind 0) and Query read requests (kind 1: opcode 0x05 READ FLAG, 0x03 READ ATTRIBUTE).
  // Read-only: no data segment, no state change on the device. Returns the UPIU response byte and
  // the 32-bit value field (attributes) or flag byte via *value; ocs 0xf means no completion.
  auto query = [&] (unsigned kind, uint8_t opcode, uint8_t idn, uint32_t *value, uint32_t *respByte) -> uint32_t {
    uint8_t *utrd = virt;
    uint8_t *ucd = virt + ucdOffset;
    zero32 (utrd, 32);
    zero32 (ucd, 1024 + 16);
    if (kind == 0) {
      ucd[0] = 0x00;                // NOP OUT
    } else {
      ucd[0] = 0x16;                // QUERY REQUEST
      ucd[5] = 0x01;                // query function (header dword 1, byte 1): standard read request
      ucd[12] = opcode;
      ucd[13] = idn;
    }
    ucd[3] = 1;                     // task tag
    uint64_t ucdPhys = phys + ucdOffset;
    *reinterpret_cast<uint32_t *> (utrd + 0) = 0x10000000U;
    *reinterpret_cast<uint32_t *> (utrd + 8) = 0x0fU;
    *reinterpret_cast<uint32_t *> (utrd + 16) = static_cast<uint32_t> (ucdPhys);
    *reinterpret_cast<uint32_t *> (utrd + 20) = static_cast<uint32_t> (ucdPhys >> 32);
    *reinterpret_cast<uint32_t *> (utrd + 24) = (512U >> 2) | ((rspOffset >> 2) << 16);
    *reinterpret_cast<uint32_t *> (utrd + 28) = (prdtOffset >> 2) << 16;
    cacheClean (virt, 16384);
    write32 (hc, 0x20, 0xffffffffU);
    write32 (hc, 0x58, 1);
    bool done = false;
    for (unsigned loops = 0; loops < 100000; loops++) {
      if (!(read32 (hc, 0x58) & 1)) {
        done = true;
        break;
      }
      IODelay (1);
    }
    cacheCleanInvalidate (virt, 16384);
    if (!done) {
      return 0xf;
    }
    const uint8_t *rsp = ucd + rspOffset;
    *respByte = (uint32_t (rsp[0]) << 8) | rsp[6];     // response type << 8 | query/NOP response
    *value = getBe32 (rsp + 20);
    return *reinterpret_cast<volatile uint32_t *> (utrd + 8) & 0xff;
  };
  // Device state before waking it. Hardware 2026-10-17: NOP OUT answers, fDeviceInit = 0 (done),
  // bCurrentPowerMode = 0x33 (UFS-PowerDown) and every LU returns NOT READY. Linux resumes the
  // device with START STOP UNIT (power condition ACTIVE) to the UFS Device well-known LU 0xD0
  // (ufshcd_set_dev_pwr_mode); no user data is touched.
  uint32_t powerBefore = 0, powerAfter = 0, queryResp = 0, ignoredValue = 0;
  uint32_t wakeStatus = 0xee;
  if (level == 5 || level == 9) {
    query (1, 0x03, 0x02, &powerBefore, &queryResp);     // READ ATTRIBUTE bCurrentPowerMode
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };          // START STOP UNIT, power condition ACTIVE
    UfsResult w = command (0xd0, ssu, 6, false, 0);
    wakeStatus = (w.ocs << 16) | (w.response << 8) | w.scsi;
    for (unsigned settle = 0; settle < 50; settle++) {
      IODelay (1000);
    }
    query (1, 0x03, 0x02, &powerAfter, &queryResp);
    (void) ignoredValue;
  }
  if (level == 4) {
    uint8_t tur0[6] = { 0, 0, 0, 0, 0, 0 };
    UfsResult t = command (0, tur0, 6, false, 0);
    const uint8_t *rsp0 = virt + ucdOffset + rspOffset;
    panic ("NATIVE_UFS adopt: TEST UNIT READY lun0\n"
           "ocs=%x resp=%02x scsi=%02x\n"
           "phys=%llx hcs=%08x->%08x smmu idx/bank=%08x cbar=%08x\n"
           "is=%08x db=%08x rsr=%08x\n"
           "rsp=%08x %08x",
           t.ocs, t.response, t.scsi, phys, hcsBefore, read32 (hc, 0x30), smmuReport[0], smmuReport[1],
           read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), getBe32 (rsp0), getBe32 (rsp0 + 4));
  }
  uint32_t hcsAfter = 0;
  const unsigned maxLun = 6;
  uint32_t status[maxLun], last[maxLun], blockSize[maxLun], senseOf[maxLun];
  for (unsigned lun = 0; lun < maxLun; lun++) {
    status[lun] = 0xee; last[lun] = 0; blockSize[lun] = 0; senseOf[lun] = 0;
    uint8_t tur[6] = { 0x00, 0, 0, 0, 0, 0 };
    UfsResult r = { 0xf, 0xff, 0xff, 0, 0, 0 };
    for (unsigned attempt = 0; attempt < 3; attempt++) {
      r = command (static_cast<uint8_t> (lun), tur, 6, false, 0);
      if (lun == 0 && attempt == 0 && r.ocs == 0xf) {
        // Never completed: stop here and report instead of waiting out every command.
        const uint8_t *rsp = virt + ucdOffset + rspOffset;
        panic ("NATIVE_UFS adopt: first command did not complete\n"
               "phys=%llx hcs=%08x->%08x\n"
               "is=%08x db=%08x rsr=%08x ba=%08x\n"
               "utrd2=%08x rsp=%08x %08x",
               phys, hcsBefore, read32 (hc, 0x30),
               read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), read32 (hc, 0x50),
               *reinterpret_cast<volatile uint32_t *> (virt + 8), getBe32 (rsp), getBe32 (rsp + 4));
      }
      if (r.ocs == 0 && r.scsi == 0) {
        break;
      }
    }
    uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    r = command (static_cast<uint8_t> (lun), cap, 10, true, 8);
    status[lun] = (r.ocs << 16) | (r.response << 8) | r.scsi;
    senseOf[lun] = r.sense;
    if (r.ocs == 0 && r.scsi == 0) {
      last[lun] = getBe32 (virt + dataOffset);
      blockSize[lun] = getBe32 (virt + dataOffset + 4);
    }
  }
  if (level == 9) {
    // Read-only GPT survey of LUN 0 (4096-byte blocks): header fields and CRCs for before/after
    // comparison, then every non-empty entry among the first 64. Nothing is written.
    if (blockSize[0] != 4096) {
      panic ("NATIVE_UFS gpt: unexpected LUN0 block size %08x", blockSize[0]);
    }
    uint8_t rd[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    UfsResult h = command (0, rd, 10, true, 4096);
    if (h.ocs != 0 || h.scsi != 0) {
      panic ("NATIVE_UFS gpt: header read failed ocs=%x resp=%x scsi=%x", h.ocs, h.response, h.scsi);
    }
    const uint8_t *hdr = virt + dataOffset;
    auto le32 = [] (const uint8_t *at) -> uint32_t {
      return uint32_t (at[0]) | (uint32_t (at[1]) << 8) | (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
    };
    auto le64 = [&] (const uint8_t *at) -> uint64_t {
      return uint64_t (le32 (at)) | (uint64_t (le32 (at + 4)) << 32);
    };
    uint32_t sig0 = le32 (hdr), sig1 = le32 (hdr + 4), hdrCrc = le32 (hdr + 16), entCrc = le32 (hdr + 88);
    uint64_t curLba = le64 (hdr + 24), bakLba = le64 (hdr + 32), firstUsable = le64 (hdr + 40), lastUsable = le64 (hdr + 48);
    uint64_t entLba = le64 (hdr + 72);
    uint32_t entCount = le32 (hdr + 80), entSize = le32 (hdr + 84);
    uint32_t guid0 = le32 (hdr + 56), guid1 = le32 (hdr + 60), guid2 = le32 (hdr + 64), guid3 = le32 (hdr + 68);
    if (sig0 != 0x20494645U || sig1 != 0x54524150U || entSize != 128 || entLba > 64) {
      panic ("NATIVE_UFS gpt: unexpected header sig=%08x %08x esz=%u elba=%llx", sig0, sig1, entSize, entLba);
    }
    uint32_t shown = entCount > 64 ? 64 : entCount;
    uint32_t blocks = (shown * 128 + 4095) / 4096;
    uint8_t rd2[10] = { 0x28, 0, 0, 0, 0, uint8_t (entLba), 0, 0, uint8_t (blocks), 0 };
    UfsResult e = command (0, rd2, 10, true, blocks * 4096);
    if (e.ocs != 0 || e.scsi != 0) {
      panic ("NATIVE_UFS gpt: entries read failed ocs=%x scsi=%x", e.ocs, e.scsi);
    }
    const uint8_t *ents = virt + dataOffset;
    static char text[3400];
    int used = snprintf (text, sizeof (text),
                         "NATIVE_UFS gpt LUN0 hdr crc=%08x ents crc=%08x cnt=%u\n"
                         "cur=%llx backup=%llx usable=%llx..%llx elba=%llx\n"
                         "disk guid %08x %08x %08x %08x last=%08x\n",
                         hdrCrc, entCrc, entCount, curLba, bakLba, firstUsable, lastUsable, entLba,
                         guid0, guid1, guid2, guid3, last[0]);
    unsigned column = 0, listed = 0;
    for (uint32_t index = 0; index < shown && used < int (sizeof (text)) - 80; index++) {
      const uint8_t *entry = ents + index * 128;
      bool empty = true;
      for (unsigned b = 0; b < 16; b++) {
        if (entry[b]) { empty = false; break; }
      }
      if (empty) {
        continue;
      }
      char name[14];
      for (unsigned c = 0; c < 13; c++) {
        uint8_t lo = entry[56 + 2 * c], hi = entry[57 + 2 * c];
        name[c] = (hi == 0 && lo >= 32 && lo < 127) ? char (lo) : (lo ? '?' : 0);
        if (!name[c]) { name[c] = 0; break; }
      }
      name[13] = 0;
      used += snprintf (text + used, sizeof (text) - used, "%u:%s %llx-%llx%s", index, name,
                        le64 (entry + 32), le64 (entry + 40), (++column % 3) ? "  " : "\n");
      listed++;
    }
    snprintf (text + used, sizeof (text) - used, "\nlisted=%u", listed);
    panic ("%s", text);
  }
  // INQUIRY of LUN 0: vendor/product bytes 8..23.
  uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
  UfsResult inq = command (0, inquiry, 6, true, 36);
  uint32_t inqWord[4] = { 0, 0, 0, 0 };
  for (unsigned index = 0; index < 4; index++) {
    inqWord[index] = getBe32 (virt + dataOffset + 8 + index * 4);
  }
  // LBA 1 of LUN 0 holds the GPT header when the block size is 4096 (or 512).
  uint32_t gptStatus = 0xee, sig0 = 0, sig1 = 0;
  if (blockSize[0] >= 512 && blockSize[0] <= 4096) {
    uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    UfsResult g = command (0, read10, 10, true, blockSize[0]);
    gptStatus = (g.ocs << 16) | (g.response << 8) | g.scsi;
    if (g.ocs == 0 && g.scsi == 0) {
      sig0 = *reinterpret_cast<uint32_t *> (virt + dataOffset);
      sig1 = *reinterpret_cast<uint32_t *> (virt + dataOffset + 4);
    }
  }
  hcsAfter = read32 (hc, 0x30);
  panic ("NATIVE_UFS adopt+read\n"
         "hcs %08x->%08x pwr %08x->%08x ssu=%06x\n"
         "L0 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L1 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L2 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L3 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L4 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L5 st=%06x last=%08x bs=%08x sns=%06x\n"
         "inq st=%06x %08x %08x %08x %08x\n"
         "inq sns=%06x slen=%x seg=%x\n"
         "rsp0 %08x %08x %08x %08x\n"
         "rsp8 %08x %08x %08x %08x\n"
         "gpt st=%06x sig=%08x %08x",
         hcsBefore, hcsAfter, powerBefore, powerAfter, wakeStatus,
         status[0], last[0], blockSize[0], senseOf[0], status[1], last[1], blockSize[1], senseOf[1],
         status[2], last[2], blockSize[2], senseOf[2], status[3], last[3], blockSize[3], senseOf[3],
         status[4], last[4], blockSize[4], senseOf[4], status[5], last[5], blockSize[5], senseOf[5],
         (inq.ocs << 16) | (inq.response << 8) | inq.scsi, inqWord[0], inqWord[1], inqWord[2], inqWord[3],
         inq.sense, inq.senseLen, inq.seg,
         getBe32 (virt + ucdOffset + rspOffset), getBe32 (virt + ucdOffset + rspOffset + 4),
         getBe32 (virt + ucdOffset + rspOffset + 8), getBe32 (virt + ucdOffset + rspOffset + 12),
         getBe32 (virt + ucdOffset + rspOffset + 32), getBe32 (virt + ucdOffset + rspOffset + 36),
         getBe32 (virt + ucdOffset + rspOffset + 40), getBe32 (virt + ucdOffset + rspOffset + 44),
         gptStatus, sig0, sig1);
}

// Read-only dump of the request list the firmware left (native-ufs-probe=6): shows where
// the firmware placed its descriptors, to compare with an allocation the controller's DMA
// is known to reach. Nothing is written and no register is touched except reads.
static uint32_t dumpWord (const volatile uint32_t *base, unsigned index)
{
  return base[index];
}

void NativeUfs::dumpFirmwareList (void)
{
  OSSharedPtr<IOMemoryMap> hcMap, listMap, ucdMap;
  uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
  if (!hc) {
    panic ("NATIVE_UFS dump: controller map failed");
  }
  uint64_t listPhys = (uint64_t (read32 (hc, 0x54)) << 32) | read32 (hc, 0x50);
  uint32_t db = read32 (hc, 0x58), rsr = read32 (hc, 0x60), hcs = read32 (hc, 0x30);
  IOMemoryDescriptor *listDesc = IOMemoryDescriptor::withPhysicalAddress (listPhys, 4096, kIODirectionIn);
  if (!listDesc) {
    panic ("NATIVE_UFS dump: list descriptor failed phys=%llx", listPhys);
  }
  listMap = OSSharedPtr<IOMemoryMap> (listDesc->map (kIOMapAnywhere), OSNoRetain);
  listDesc->release ();
  if (!listMap) {
    panic ("NATIVE_UFS dump: list map failed phys=%llx", listPhys);
  }
  const volatile uint32_t *utrd = reinterpret_cast<const volatile uint32_t *> (listMap->getVirtualAddress ());
  uint64_t ucdPhys = (uint64_t (dumpWord (utrd, 5)) << 32) | dumpWord (utrd, 4);
  uint32_t ucd0 = 0, ucd1 = 0, ucd2 = 0, ucd3 = 0;
  if (ucdPhys) {
    IOMemoryDescriptor *ucdDesc = IOMemoryDescriptor::withPhysicalAddress (ucdPhys, 4096, kIODirectionIn);
    if (ucdDesc) {
      ucdMap = OSSharedPtr<IOMemoryMap> (ucdDesc->map (kIOMapAnywhere), OSNoRetain);
      ucdDesc->release ();
      if (ucdMap) {
        const volatile uint32_t *ucd = reinterpret_cast<const volatile uint32_t *> (ucdMap->getVirtualAddress ());
        ucd0 = dumpWord (ucd, 0); ucd1 = dumpWord (ucd, 1); ucd2 = dumpWord (ucd, 2); ucd3 = dumpWord (ucd, 3);
      }
    }
  }
  panic ("NATIVE_UFS dump: firmware list\n"
         "list=%llx hcs=%08x db=%08x rsr=%08x\n"
         "utrd0 %08x %08x %08x %08x\n"
         "utrd0 %08x %08x %08x %08x\n"
         "utrd1 %08x %08x %08x %08x\n"
         "ucd=%llx %08x %08x %08x %08x",
         listPhys, hcs, db, rsr,
         dumpWord (utrd, 0), dumpWord (utrd, 1), dumpWord (utrd, 2), dumpWord (utrd, 3),
         dumpWord (utrd, 4), dumpWord (utrd, 5), dumpWord (utrd, 6), dumpWord (utrd, 7),
         dumpWord (utrd, 8), dumpWord (utrd, 9), dumpWord (utrd, 10), dumpWord (utrd, 11),
         ucdPhys, ucd0, ucd1, ucd2, ucd3);
}

// Read-only dump of the apps SMMU (apps_smmu@15000000) global register space and the stream
// match entry for the UFS stream ID 0x60 (native-ufs-probe=7). SMMUv2 layout: sCR0 0x0,
// IDR0 0x20, sGFSR 0x48, SMR(n) 0x800+4n, S2CR(n) 0xc00+4n. Nothing is written.
void NativeUfs::inspectSmmu (void)
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0x1000, map);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  uint32_t scr0 = read32 (smmu, 0x00), idr0 = read32 (smmu, 0x20), idr1 = read32 (smmu, 0x24);
  uint32_t gfsr = read32 (smmu, 0x48);
  unsigned count = idr0 & 0xff;
  if (count > 128) {
    count = 128;
  }
  unsigned match = 0xffff, valid = 0, typeCount[4] = { 0, 0, 0, 0 };
  uint32_t matchSmr = 0, matchS2cr = 0;
  for (unsigned index = 0; index < count; index++) {
    uint32_t smr = read32 (smmu, 0x800 + 4 * index);
    uint32_t s2cr = read32 (smmu, 0xc00 + 4 * index);
    if (smr & 0x80000000U) {
      valid++;
      typeCount[(s2cr >> 16) & 3]++;
      uint32_t mask = (smr >> 16) & 0x7fff;
      if (match == 0xffff && (((smr ^ 0x60U) & 0x7fffU) & ~mask) == 0) {
        match = index;
        matchSmr = smr;
        matchS2cr = s2cr;
      }
    }
  }
  panic ("NATIVE_UFS smmu\n"
         "scr0=%08x idr0=%08x idr1=%08x gfsr=%08x\n"
         "nsmr=%u valid=%u types t0=%u t1=%u t2=%u\n"
         "sid0x60 match=%x smr=%08x s2cr=%08x",
         scr0, idr0, idr1, gfsr, count, valid, typeCount[0], typeCount[1], typeCount[2],
         match, matchSmr, matchS2cr);
}

// Read-only context-bank survey (native-ufs-probe=8): CBAR/CBA2R in gr1 and SCTLR/TTBR0 in the
// context banks, to find a bank the hypervisor leaves to us. Nothing is written.
void NativeUfs::inspectContextBanks (void)
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, map);
  if (!smmu) {
    panic ("NATIVE_UFS cb: map failed");
  }
  uint32_t idr1 = read32 (smmu, 0x24);
  unsigned numCb = idr1 & 0xff;
  unsigned globalPages = 1U << (((idr1 >> 28) & 7) + 1);
  if (globalPages > 128 || numCb > 128 || (globalPages + numCb) * 0x1000 > 0xd0000) {
    panic ("NATIVE_UFS cb: unexpected geometry idr1=%08x", idr1);
  }
  unsigned cbarZero = 0, sctlrZero = 0, mSet = 0, bothZero = 0;
  uint32_t firstZero = 0xffff;
  for (unsigned index = 0; index < numCb; index++) {
    uint32_t cbar = read32 (smmu, 0x1000 + 4 * index);
    uint32_t sctlr = read32 (smmu + (globalPages + index) * 0x1000, 0x0);
    cbarZero += cbar == 0;
    sctlrZero += sctlr == 0;
    mSet += (sctlr & 1) != 0;
    if (cbar == 0 && sctlr == 0) {
      bothZero++;
      if (firstZero == 0xffff) {
        firstZero = index;
      }
    }
  }
  uintptr_t cb0 = smmu + globalPages * 0x1000, cb1 = cb0 + 0x1000, cbLast = cb0 + (numCb - 1) * 0x1000;
  panic ("NATIVE_UFS cb survey\n"
         "ncb=%u pages=%u cbar0=%u sctlr0=%u msset=%u both0=%u first0=%x\n"
         "cbar[0]=%08x cbar[1]=%08x cbar[%u]=%08x\n"
         "sctlr[0]=%08x sctlr[1]=%08x sctlr[last]=%08x\n"
         "cba2r[0]=%08x cba2r[1]=%08x",
         numCb, globalPages, cbarZero, sctlrZero, mSet, bothZero, firstZero,
         read32 (smmu, 0x1000), read32 (smmu, 0x1004), numCb - 1, read32 (smmu, 0x1000 + 4 * (numCb - 1)),
         read32 (cb0, 0), read32 (cb1, 0), read32 (cbLast, 0),
         read32 (smmu, 0x1800), read32 (smmu, 0x1804));
}

// ======================================================================================
// Root media: the live controller as a read-only block device.
//
// One shared host serves every block device: up to kSlots commands in flight, each in its own request slot
// (RootTransportLayout.h), polled; interrupts stay masked. The DMA region is ordinary cached, physically contiguous kernel memory with
// explicit cache maintenance around each command (the stream is mapped through the reserved
// apps_smmu bypass context bank by smmuBypassStream, so physical addresses are used as-is).
// Nothing here writes to the storage: only INQUIRY-class and READ(10) commands are built, plus
// the single START STOP UNIT that wakes the device from the power-down the firmware left.
// ======================================================================================
namespace {
#include "RootTransportLayout.h"

struct UfsHost {
  OSSharedPtr<IOMemoryMap> hcMap, smmuMap;
  uintptr_t hc;
  IOBufferMemoryDescriptor *dma;
  uint8_t *virt;
  uint64_t phys;
  uint32_t dmaBytes;
  IOLock *lock;   // the command slot pool's (RootCommands.h)
  bool ready;
  bool poisoned = false;
  uintptr_t smmu = 0, cb = 0;
};
UfsHost gHost = { OSSharedPtr<IOMemoryMap> (), OSSharedPtr<IOMemoryMap> (), 0, nullptr, nullptr, 0, 0, nullptr, false };
// Command slots in use (1 to kSlots), for the log.
extern "C" volatile uint32_t native_ufs_slots = 0;

static IOReturn recordTransport (IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  gRootDiagnostic.ocs = ocs;
  if (result != kIOReturnSuccess && gRootDiagnosticEnabled) {
    // Read only the already mapped HC's status/registers used by this driver.
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
    gRootDiagnostic.doorbell = read32 (gHost.hc, 0x58);
    gRootDiagnostic.interrupt = read32 (gHost.hc, 0x20);
    gRootDiagnostic.list = uint64_t (read32 (gHost.hc, 0x50)) |
                          (uint64_t (read32 (gHost.hc, 0x54)) << 32);
    gRootDiagnostic.runStop = read32 (gHost.hc, 0x60);
    gRootDiagnostic.ahit = read32 (gHost.hc, 0x18);
    for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.uic[i] = read32 (gHost.hc, 0x38 + 4 * i);
    if (gHost.smmu && gHost.cb) {
      gRootDiagnostic.gfsr = read32 (gHost.smmu, 0x48);
      gRootDiagnostic.gsyn = read32 (gHost.smmu, 0x54);
      gRootDiagnostic.fsr = read32 (gHost.cb, 0x58);
      gRootDiagnostic.fsyn = read32 (gHost.cb, 0x68);
      gRootDiagnostic.far = uint64_t (read32 (gHost.cb, 0x60)) |
                            (uint64_t (read32 (gHost.cb, 0x64)) << 32);
    }
  }
  return result;
}

static void cacheCivac (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Device-owned cache lines must not be written back over DMA results. All
// callers use complete 64-byte lines from the dedicated page-aligned buffer.
static void cacheInvalidate (const void *start, size_t length)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  uintptr_t address = reinterpret_cast<uintptr_t> (start);
  for (uintptr_t end = address + length; address < end; address += 64) {
    __asm__ volatile ("dc ivac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void hostWrite (unsigned offset, uint32_t value)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gHost.hc, offset, value);
  __asm__ volatile ("dsb sy" ::: "memory");
}

#include "RootQueue.h"

// Completion interrupt hooks of RootCommands.h; defined after it.
static uint32_t hostCompletionCount (unsigned slot);
static bool hostInterruptMode (void);
static void hostWaitCompletion (unsigned slot, uint32_t seen, uint64_t started);
static void hostWaitDone (uint64_t started);
static uint64_t hostDmaAddress (const void *va);
#include "RootCommands.h"

// ---- Completion interrupt ------------------------------------------------------------------------------------------
// The controller's interrupt (GIC SPI 265, level) with only UTRCS enabled (IE bit 0), as in UFSHCI's legacy doorbell mode under Linux
// ufshcd. The handler clears UTRCS, then, like ufshcd's completion of the requests whose doorbell bits cleared, finds the outstanding
// commands whose bits are clear and wakes only their waiters; a waiter rechecks its own doorbell bit and OCS. A wait polls for its
// spin budget (gSpinUs) first and then sleeps until its slot's completion (CompletionWait.h; boot-arg native-ufs-wait selects how it
// polls). Bring-up and the panic path always poll; without the interrupt (attach refused) the host polls as before.
//
// Why the polls spin: a command takes about 100 us here, and on this platform a thread that blocks takes 30 to 37 us on average to run
// again, whether a timer or the completion interrupt ends the block (pauses and wakes of the log's UFS row). Sleeping on every
// completion halved small-block throughput against polling, and pausing with IODelay (2) blocks on a timer 2 to 3 times per command,
// because the cores register no idle exit latency (ml_delay_should_spin), and each block is charged to the task as a wakeup.
extern "C" IOReturn armcpu_spi_attach (uint32_t, bool, void (*) (void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" IOReturn armcpu_spi_mask (uint32_t, bool) __attribute__((weak_import));
constexpr uint32_t kUfsIrq = 265 + 32;
static volatile uint32_t gIrqCount = 0;
static bool gIrqMode = false;
extern "C" volatile uint64_t native_ufs_irq_sleeps = 0, native_ufs_irqs = 0;
extern "C" volatile uint32_t native_ufs_irq_mode = 0;

static uint32_t hostCompletionCount (unsigned slot) { return __atomic_load_n (&gSlotGen[slot], __ATOMIC_ACQUIRE); }
static bool hostInterruptMode (void) { return gIrqMode && !gHostPanic; }
static uint64_t gSpinTicks = 0;
static uint32_t gSpinTicksUs = ~0U;
static uint32_t gWaitMode = kUfsWaitSpin;   // boot-arg native-ufs-wait (CompletionWait.h)
// Completion wait statistics, for the log (native_ufs_stats_text).
// A paced pause is IODelay (2). XNU's delay spins only when the delay is shorter than the idle exit latency the CPU was registered with
// (ml_delay_should_spin, from ml_processor_info_t.powergate_latency); on a CPU registered without one it blocks the thread on a timer,
// so a pause then lasts longer than 2 us and its end is charged to the calling task as an interrupt wakeup.
// gWaitPauses and gWaitPauseTicks record what the paced pauses cost on the running platform. gSlotSpin counts the spinning pauses and
// their time per slot, written only by the slot's waiter, so a spin touches no shared cache line. gWaitYields counts the spins that
// ended in a sleep because a preemption was pending, gWaitRange the time from the start of a wait to the poll that saw its command
// complete. gIrqWakes, gIrqWakeTicks and gIrqWakeMaxTicks
// record the sleeps the interrupt ended and the time from the handler first seeing the completion to the waiter running again;
// gIrqTimeouts the sleeps that ran into their timeout.
// gSlotDoneAt holds that first sighting: the waiter clears it before it sleeps and the handler fills it only while it is clear, since
// every later interrupt sees the same command complete again until its waiter has run.
static volatile uint64_t gWaitPauses = 0, gWaitPauseTicks = 0, gIrqWakes = 0, gIrqWakeTicks = 0, gIrqWakeMaxTicks = 0, gIrqTimeouts = 0;
static volatile uint64_t gWaitYields = 0, gWaitRange[kUfsWaitRanges] = {};
struct alignas (64) UfsSlotSpin { volatile uint64_t pauses, ticks; };
static UfsSlotSpin gSlotSpin[kSlots];
static volatile uint64_t gSlotDoneAt[kSlots] = {};
static void hostWaitCompletion (unsigned slot, uint32_t seen, uint64_t started)
{
  const uint32_t spinUs = *static_cast<volatile uint32_t *> (&gSpinUs);
  if (spinUs != gSpinTicksUs) { nanoseconds_to_absolutetime (uint64_t (spinUs) * 1000, &gSpinTicks); gSpinTicksUs = spinUs; }
  const uint64_t now = mach_absolute_time ();
  const bool canBlock = preemption_enabled ();   // interrupts on and preemption level 0
  // A stale read after a migration only moves the switch to sleeping by one pause.
  const bool preemptPending = canBlock && (*ast_pending () & AST_PREEMPTION) != 0;
  const UfsWaitStep step = ufsWaitStep (gWaitMode, hostInterruptMode (), canBlock, preemptPending, now - started, gSpinTicks);
  if (step == UfsWaitStep::Spin) {
    __builtin_arm_wfe ();
    UfsSlotSpin &spin = gSlotSpin[slot];
    spin.pauses = spin.pauses + 1;
    spin.ticks = spin.ticks + (mach_absolute_time () - now);
    return;
  }
  if (step == UfsWaitStep::Pause) {
    IODelay (2);
    __atomic_fetch_add (&gWaitPauses, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add (&gWaitPauseTicks, mach_absolute_time () - now, __ATOMIC_RELAXED);
    return;
  }
  if (preemptPending && gWaitMode == kUfsWaitSpin && now - started < gSpinTicks) __atomic_fetch_add (&gWaitYields, 1, __ATOMIC_RELAXED);
  __atomic_store_n (&gSlotDoneAt[slot], 0, __ATOMIC_RELAXED);
  assert_wait_timeout ((event_t) (uintptr_t) &gSlotGen[slot], THREAD_UNINT, 1000, NSEC_PER_USEC);
  if (hostCompletionCount (slot) != seen) {
    clear_wait (current_thread (), THREAD_AWAKENED);
    return;
  }
  __atomic_fetch_add (&native_ufs_irq_sleeps, 1, __ATOMIC_RELAXED);
  thread_block (THREAD_CONTINUE_NULL);
  if (hostCompletionCount (slot) == seen) {
    __atomic_fetch_add (&gIrqTimeouts, 1, __ATOMIC_RELAXED);
    return;
  }
  const uint64_t done = __atomic_load_n (&gSlotDoneAt[slot], __ATOMIC_RELAXED), woke = mach_absolute_time ();
  if (!done) return;
  const uint64_t late = woke > done ? woke - done : 0;
  __atomic_fetch_add (&gIrqWakes, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add (&gIrqWakeTicks, late, __ATOMIC_RELAXED);
  uint64_t longest = __atomic_load_n (&gIrqWakeMaxTicks, __ATOMIC_RELAXED);
  while (late > longest && !__atomic_compare_exchange_n (&gIrqWakeMaxTicks, &longest, late, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
}

static void hostWaitDone (uint64_t started)
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds (mach_absolute_time () - started, &ns);
  __atomic_fetch_add (&gWaitRange[ufsWaitRange (ns / 1000)], 1, __ATOMIC_RELAXED);
}

static void hostInterrupt (void *)
{
  if (read32 (gHost.hc, 0x20) & 1) {
    write32 (gHost.hc, 0x20, 1);
    __asm__ volatile ("dsb sy" ::: "memory");
  }
  __atomic_add_fetch (&gIrqCount, 1, __ATOMIC_RELEASE);
  __atomic_fetch_add (&native_ufs_irqs, 1, __ATOMIC_RELAXED);
  uint32_t done = __atomic_load_n (&gOutstanding, __ATOMIC_ACQUIRE) & ~read32 (gHost.hc, 0x58);
  const uint64_t now = done ? mach_absolute_time () : 0;
  while (done) {
    const unsigned entry = static_cast<unsigned> (__builtin_ctz (done));
    done &= done - 1;
    const int slot = gEntrySlot[entry];
    if (slot < 0) continue;
    uint64_t clear = 0;
    __atomic_compare_exchange_n (&gSlotDoneAt[slot], &clear, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    __atomic_add_fetch (&gSlotGen[slot], 1, __ATOMIC_RELEASE);
    thread_wakeup ((event_t) (uintptr_t) &gSlotGen[slot]);
  }
}

// Zero copy (RootCommands.h): a kernel page's physical address, if the storage stream can reach it (gDmaLimit, from the SMMU's
// bypass width and the controller's 64-bit addressing, set at bring-up; 0 until then, so no zero copy).
static uint64_t gDmaLimit = 0;
static uint64_t hostDmaAddress (const void *va)
{
  const uint64_t address = ml_vtophys (reinterpret_cast<vm_offset_t> (va));
  return address && address < gDmaLimit && gDmaLimit - address >= 4096 ? address : 0;
}
extern "C" void native_ufs_set_zero_copy (int on) { gZeroCopy = on != 0; }
extern "C" void native_ufs_set_spin (uint32_t us) { gSpinUs = us; }

static void hostEnableInterrupt (void)
{
  if (gIrqMode || !&armcpu_spi_attach || !&armcpu_spi_mask) return;
  uint32_t mode = kUfsWaitSpin, spinUs = 0;
  if (PE_parse_boot_argn ("native-ufs-wait", &mode, sizeof (mode)) && mode <= kUfsWaitSleep) gWaitMode = mode;
  if (PE_parse_boot_argn ("native-ufs-spin-us", &spinUs, sizeof (spinUs))) gSpinUs = spinUs;
  if (armcpu_spi_attach (kUfsIrq, false, hostInterrupt, nullptr, nullptr, nullptr) != kIOReturnSuccess) return;
  hostWrite (0x24, 1);                      // UTRCE only
  if (armcpu_spi_mask (kUfsIrq, false) != kIOReturnSuccess) { hostWrite (0x24, 0); return; }
  gIrqMode = true;
  native_ufs_irq_mode = 1;
}

// The request list on a write-combined page of its own (RootTransportLayout.h): every entry serves a slot, and region A's slots use
// entries 0..n-1. With 32 entries and region A's 16 slots, region B adds slots 16..31. Any refusal (allocation, address beyond the
// stream's bypass width or 32 bits without 64-bit addressing, mapping) keeps region A's list and entries 0, 2, ...
static OSSharedPtr<IOMemoryMap> gListMap;
static IOBufferMemoryDescriptor *gListDma, *gRegionB;
extern "C" volatile uint32_t native_ufs_list_mode = 0;   // 0 list in region A (entry 2s), 1 own write-combined page, 2 also region B
static bool hostReachable (uint64_t phys, uint64_t bytes, bool wide)
{
  if (!wide && phys + bytes > 0x100000000ULL) return false;
  if (!gHost.smmu) return false;
  const uint32_t idr2 = read32 (gHost.smmu, 0x28);
  for (unsigned shift = 0; shift <= 8; shift += 4) {
    const unsigned bits = ufsSmmuAddressBits ((idr2 >> shift) & 15);
    if (!bits || phys >= (1ULL << bits) || bytes > (1ULL << bits) - phys) return false;
  }
  return true;
}
static IOBufferMemoryDescriptor *hostContiguous (uint32_t bytes, IOOptionBits cache, bool wide, uint8_t **virt, uint64_t *phys)
{
  IOBufferMemoryDescriptor *dma = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone) | cache, bytes, 4096);
  if (!dma) return nullptr;
  IOByteCount contiguous = 0;
  *virt = static_cast<uint8_t *> (dma->getBytesNoCopy ());
  *phys = dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  if (!*virt || !*phys || (*phys & 4095) || contiguous < bytes || !hostReachable (*phys, bytes, wide)) {
    dma->release ();
    return nullptr;
  }
  bzero (*virt, bytes);
  cacheCivac (*virt, bytes);
  return dma;
}
static void hostSlotsUpgrade (unsigned regionSlots, unsigned entries, bool wide)
{
  if (gListDma || regionSlots < 2 || entries < regionSlots) return;
  uint8_t *cached = nullptr; uint64_t listPhys = 0;
  IOBufferMemoryDescriptor *list = hostContiguous (4096, kIOMapWriteCombineCache, wide, &cached, &listPhys);
  if (!list) return;
  OSSharedPtr<IOMemoryMap> map (list->createMappingInTask (kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
  uint8_t *listVirt = map ? reinterpret_cast<uint8_t *> (map->getVirtualAddress ()) : nullptr;
  if (!listVirt) { list->release (); return; }
  for (unsigned i = 0; i < 4096; ++i) listVirt[i] = 0;
  __asm__ volatile ("dsb sy" ::: "memory");
  gListDma = list; gListMap = map; gUtrlPhys = listPhys;
  for (unsigned e = 0; e < 32; ++e) gEntrySlot[e] = -1;
  for (unsigned s = 0; s < regionSlots; ++s)
    hostSlotGeometry (s, listVirt + 32 * s, s, gHost.virt + ufsSlotUcdOffset (s), gHost.phys + ufsSlotUcdOffset (s),
                      gHost.virt + ufsSlotDataOffset (s), gHost.phys + ufsSlotDataOffset (s));
  gListUncached = true;
  unsigned total = regionSlots;
  native_ufs_list_mode = 1;
  uint8_t *b = nullptr; uint64_t bPhys = 0;
  if (regionSlots == kRegionSlots && entries >= kSlots && (gRegionB = hostContiguous (kRegionBBytes, 0, wide, &b, &bPhys))) {
    for (unsigned k = 0; k < kRegionSlots; ++k)
      hostSlotGeometry (kRegionSlots + k, listVirt + 32 * (kRegionSlots + k), kRegionSlots + k, b + k * kUcdStride,
                        bPhys + k * kUcdStride, b + kRegionBDataOffset + k * kDataMax, bPhys + kRegionBDataOffset + k * kDataMax);
    total = kSlots;
    native_ufs_list_mode = 2;
  }
  hostSlotsInit (total, gHost.lock);
  native_ufs_slots = gSlotCount;
}

// Bring the live controller up for reading: map, SMMU pass-through for the UFS stream, take over
// the request list, wake the device from UFS-PowerDown and wait for LUN 0 to be ready.
// `why` receives a short reason on failure.
static bool hostBringUp (const char **why)
{
  if (gHost.poisoned) {
    *why = "transport quarantined";
    return false;
  }
  if (gHost.ready) {
    return true;
  }
  gRootDiagnostic.stage = "controller map";
  gHost.hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, gHost.hcMap);
  if (!gHost.hc) {
    *why = "controller map";
    return false;
  }
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.cap = read32 (gHost.hc, 0x00);
    gRootDiagnostic.version = read32 (gHost.hc, 0x08);
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
  }
  gRootDiagnostic.stage = "DMA allocation";
  // All kSlots command slots when the region is physically contiguous and addressable, else slot 0's original layout alone. The
  // request list must also have entries for them (CAP.NUTRS, entries 0, 2, ...).
  const bool wide = read32 (gHost.hc, 0) & 0x01000000;
  const unsigned entries = (read32 (gHost.hc, 0) & 0x1f) + 1;
  unsigned slots = (entries + 1) / 2 < kRegionSlots ? (entries + 1) / 2 : kRegionSlots;
  gHost.lock = IOLockAlloc ();
  // Smaller regions keep the same layout for their first slots: 16, then 8, then 1.
  const uint32_t sizes[3] = { uint32_t (kDmaBytes), uint32_t (kDmaBytesEightSlots), uint32_t (kDmaBytesOneSlot) };
  const unsigned counts[3] = { kRegionSlots, 8, 1 };
  for (unsigned attempt = 0; attempt < 3 && !gHost.virt; ++attempt) {
    const uint32_t bytes = sizes[attempt];
    if (counts[attempt] > slots && counts[attempt] > 1) continue;
    IOBufferMemoryDescriptor *dma = IOBufferMemoryDescriptor::inTaskWithOptions (
      kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
      static_cast<IOOptionBits> (kIOMemoryMapperNone),
      bytes, 4096);
    if (!dma) continue;
    uint8_t *virt = static_cast<uint8_t *> (dma->getBytesNoCopy ());
    IOByteCount contiguous = 0;
    uint64_t phys = dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
    gRootDiagnostic.dma = phys;
    gRootDiagnostic.contiguous = contiguous;
    if (!virt || !phys || (phys & 4095) || contiguous < bytes || (!wide && phys + bytes > 0x100000000ULL)) {
      dma->release ();
      continue;
    }
    gHost.dma = dma; gHost.virt = virt; gHost.phys = phys; gHost.dmaBytes = bytes;
  }
  if (!gHost.virt || !gHost.lock) {
    *why = !gHost.lock ? "dma buffer" : "DMA address/contiguity";
    return false;
  }
  const unsigned regionSlots = gHost.dmaBytes == kDmaBytes ? slots : gHost.dmaBytes == kDmaBytesEightSlots ? (slots < 8 ? slots : 8) : 1;
  hostSlotGeometryRegionA (regionSlots);
  hostSlotsInit (regionSlots, gHost.lock);
  native_ufs_slots = gSlotCount;
  bzero (gHost.virt, gHost.dmaBytes);
  cacheCivac (gHost.virt, gHost.dmaBytes);
  uint32_t report[3] = { 0, 0, 0 };
  gRootDiagnostic.stage = "SMMU and request list";
  if (!hostStopQueue (why)) return false;
  smmuBypassStream (0x60, gHost.smmuMap, report, gHost.phys, gHost.dmaBytes);
  gHost.smmu = gHost.smmuMap->getVirtualAddress ();
  // The board's WiFi endpoint (wifi=2 or more) needs a pass-through stream of its own, set up here with the same code and at the same moment as the
  // storage stream: the registers of the stream matching group are not safe to touch from another thread later (a read of the first one never returned).
  uint32_t wifi = 0, ukeWifi = 3;   // WifiProbe defaults to mode 3 when uke-wifi is absent
  PE_parse_boot_argn ("wifi", &wifi, sizeof (wifi));
  PE_parse_boot_argn ("uke-wifi", &ukeWifi, sizeof (ukeWifi));
  if (wifi < 2 && ukeWifi >= 2) wifi = ukeWifi;   // this port names the Wi-Fi switch uke-wifi
  if (wifi >= 2) {
    static OSSharedPtr<IOMemoryMap> extraMap;   // kept: the fault registers are read from it later
    uint32_t extraReport[3] = { 0, 0, 0 };
    // Reuse the verified WLAN bank, then widen its exact SID 0x1401 to the
    // OEM DT match <0x1400 0x1>. All of this runs before the WPSS is started.
    constexpr uint32_t kWlanSid = 0x1400, kWlanSidMask = 1;
    constexpr uint32_t kSmrValid = 1U << 31;
    constexpr uint32_t kWlanSmr = kSmrValid | (kWlanSidMask << 16) | kWlanSid;
    smmuBypassStream (kWlanSid + 1, extraMap, extraReport, 0x9c900000ULL, 8ULL << 20);
    const uintptr_t smmuBase = extraMap->getVirtualAddress ();
    const unsigned slot = extraReport[0] & 0xffff;
    const unsigned bank = extraReport[0] >> 16;
    const unsigned groups = read32 (smmuBase, 0x20) & 0xff;
    const uint32_t oldSmr = read32 (smmuBase, 0x800 + 4 * slot);
    bool safe = oldSmr == (kSmrValid | (kWlanSid + 1)) && bank != (report[0] >> 16);
    for (unsigned i = 0; safe && i < groups; ++i) {
      if (i == slot) continue;
      const uint32_t smr = read32 (smmuBase, 0x800 + 4 * i);
      const uint32_t mask = (smr >> 16) & 0x7fff;
      if ((smr & kSmrValid) && !((smr ^ kWlanSid) & 0x7fff & ~(mask | kWlanSidMask)))
        safe = false;
    }

    // Use the beginning of the OEM WLAN DMA pool (0xb0000000 + 256 MiB).
    // The existing MSI doorbell translation occupies a separate 1 GiB region.
    constexpr uint32_t kExtraIova = 0xb0000000U, kExtraBlocks = 24, kBlock = 2U << 20;
    constexpr uint32_t kMsiIova = 0x13000000U, kMsiPa = 0x17000000U, kMsiDoorbell = 0x17110040U;
    constexpr uint32_t kPage = 4096, kTableBytes = 3 * kPage;
    const IOOptionBits options = static_cast<IOOptionBits> (kIODirectionInOut) |
      static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) | static_cast<IOOptionBits> (kIOMemoryMapperNone);
    IOBufferMemoryDescriptor *buffer = safe ? IOBufferMemoryDescriptor::inTaskWithOptions (
      kernel_task, options, (kExtraBlocks + 1) * kBlock, kBlock) : nullptr;
    IOBufferMemoryDescriptor *tables = buffer ? IOBufferMemoryDescriptor::inTaskWithOptions (
      kernel_task, options, kTableBytes, kPage) : nullptr;
    if (tables) {
      write32 (smmuBase, 0x800 + 4 * slot, kWlanSmr);
      __asm__ volatile ("dsb sy" ::: "memory");
      safe = read32 (smmuBase, 0x800 + 4 * slot) == kWlanSmr;
      if (!safe) {
        write32 (smmuBase, 0x800 + 4 * slot, oldSmr);
        __asm__ volatile ("dsb sy" ::: "memory");
      }
    }
    if (tables && safe) {
      uint8_t *bva = static_cast<uint8_t *> (buffer->getBytesNoCopy ());
      uint64_t bpa = buffer->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
      uint64_t shift = (kBlock - (bpa & (kBlock - 1))) & (kBlock - 1);
      bva += shift; bpa += shift;
      bzero (buffer->getBytesNoCopy (), (kExtraBlocks + 1) * kBlock);
      uint64_t *l1 = static_cast<uint64_t *> (tables->getBytesNoCopy ());
      uint64_t *msiL2 = l1 + 512;
      uint64_t *dmaL2 = msiL2 + 512;
      bzero (l1, kTableBytes);
      const uint64_t tablesPa = tables->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
      l1[kMsiIova >> 30] = (tablesPa + kPage) | 3;
      l1[kExtraIova >> 30] = (tablesPa + 2 * kPage) | 3;
      uint32_t attr = 0;
      PE_parse_boot_argn ("sheng-wifi-attr", &attr, sizeof (attr));
      const uint64_t share = attr == 1 ? 0 : (3ULL << 8);
      const uint64_t blockAttrs = (1ULL << 11) | (1ULL << 10) | share | (1ULL << 6) | 1;
      for (uint32_t i = 0; i < kExtraBlocks; ++i)
        dmaL2[((kExtraIova >> 21) & 511) + i] = (bpa + i * kBlock) | blockAttrs;
      msiL2[(kMsiIova >> 21) & 511] = static_cast<uint64_t> (kMsiPa) | blockAttrs;
      for (uint32_t off = 0; off < kTableBytes; off += 64)
        __asm__ volatile ("dc civac, %0" :: "r" (reinterpret_cast<uint8_t *> (l1) + off) : "memory");
      __asm__ volatile ("dsb sy" ::: "memory");
      const unsigned pages = 1U << (((read32 (smmuBase, 0x24) >> 28) & 7) + 1);
      const uintptr_t cb = smmuBase + (pages + bank) * 4096;
      write32 (cb, 0x0, 0);                               // stage 1 off while the table is set
      write32 (cb, 0x20, static_cast<uint32_t> (tablesPa)); write32 (cb, 0x24, static_cast<uint32_t> (tablesPa >> 32));
      // Match ARM SMMU's AArch64 TCR2 encoding and dpuEnable above.
      // SEP_UPSTREAM accepts zero-extended WLAN IOVAs with bit 31 set.
      write32 (cb, 0x10, 5U | (7U << 15));                // PASIZE 48-bit, SEP_UPSTREAM
      write32 (cb, 0x30, 32U | (1U << 23));               // T0SZ 32, non-cacheable walks, 4 KiB granule, no TTBR1
      write32 (cb, 0x38, attr == 1 ? 0x44 : 0xff);        // MAIR attribute 0: write-back (or non-cacheable)
      write32 (cb, 0x618, 0); write32 (cb, 0x7f0, 0);     // TLB invalidate all, sync
      __asm__ volatile ("dsb sy" ::: "memory");
      write32 (cb, 0x0, 1U | (1U << 5) | (1U << 6));      // SCTLR: M, CFRE, CFIE
      __asm__ volatile ("dsb sy; isb" ::: "memory");
      gExtraSmmu = smmuBase; gExtraCb = cb; gExtraBank = extraReport[0] >> 16;
      // Ask the context bank to translate every page of the window (stage 1, privileged read): count the pages that fault or land elsewhere than the table says.
      {
        uint32_t tested = 0, faults = 0, wrong = 0, first = 0;
        for (uint64_t iova = kExtraIova; iova < kExtraIova + static_cast<uint64_t> (kExtraBlocks) * kBlock; iova += 4096) {
          write32 (cb, 0x800, static_cast<uint32_t> (iova)); write32 (cb, 0x804, 0);
          uint32_t spin = 0;
          while ((read32 (cb, 0x8f0) & 1) && ++spin < 100000) { }
          if (spin >= 100000) { native_ufs_extra_atos[3] = 0xdead0000U | tested; break; }
          const uint32_t parLo = read32 (cb, 0x50), parHi = read32 (cb, 0x54);
          ++tested;
          if (tested == 1) { native_ufs_extra_atos[4] = parLo; native_ufs_extra_atos[5] = parHi; native_ufs_extra_atos[6] = static_cast<uint32_t> (bpa); native_ufs_extra_atos[7] = static_cast<uint32_t> (bpa >> 32); }
          if (parLo & 1) { if (!faults++) first = static_cast<uint32_t> (iova); continue; }
          const uint64_t pa = ((static_cast<uint64_t> (parHi) << 32) | parLo) & 0x0000fffffffff000ULL;
          if (pa != ((bpa + (iova - kExtraIova)) & ~0xfffULL)) { if (!wrong++ && !first) first = static_cast<uint32_t> (iova); }
        }
        native_ufs_extra_atos[0] = tested; native_ufs_extra_atos[1] = faults; native_ufs_extra_atos[2] = wrong;
        if (native_ufs_extra_atos[3] == 0) native_ufs_extra_atos[3] = first;
      }
      // The CPU reaches the buffer through a second mapping that is not cached (the buffer's own kernel mapping is cached), so what the CPU writes is in
      // memory when the endpoint reads it and what the endpoint writes is seen at once, whether or not the link is cache coherent. The cached lines are
      // written back and dropped first, and that mapping is not used again.
      for (uint64_t off = 0; off < (kExtraBlocks + 1) * static_cast<uint64_t> (kBlock); off += 64)
        __asm__ volatile ("dc civac, %0" :: "r" (static_cast<uint8_t *> (buffer->getBytesNoCopy ()) + off) : "memory");
      __asm__ volatile ("dsb sy" ::: "memory");
      IOMemoryMap *wc = buffer->createMappingInTask (kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
      native_ufs_extra_va = wc ? reinterpret_cast<uint8_t *> (wc->getVirtualAddress ()) + (bva - static_cast<uint8_t *> (buffer->getBytesNoCopy ())) : bva;
      native_ufs_extra_uncached = wc != nullptr;
      native_ufs_extra_bytes = kExtraBlocks * kBlock;
      native_ufs_extra_iova = kExtraIova;
      native_ufs_msi_iova = kMsiIova + (kMsiDoorbell - kMsiPa);
      native_ufs_extra_stream = kSmrValid | extraReport[0];
      printf ("UKE WIFI: WLAN SMMU smr=%08x bank=%u dma=%08x msi=%08x tcr2=%08x atos=%u/%u/%u\n",
              kWlanSmr, bank, kExtraIova, native_ufs_msi_iova, read32 (cb, 0x10),
              native_ufs_extra_atos[0], native_ufs_extra_atos[1], native_ufs_extra_atos[2]);
    } else {
      if (tables) tables->release ();
      if (buffer) buffer->release ();
      printf ("UKE WIFI: WLAN SMMU window unavailable safe=%u smr=%08x bank=%u\n",
              unsigned (safe), oldSmr, bank);
    }
  }
  // The display's streams (dpu=2 or more, or dpu-scanout), read here for the same reason: what the boot loader left for 0x1c00 and 0x1c02 (`iommus =
  // <&apps_smmu 0x1c00 0x2>`), the first step of letting the display fetch the compositor's pages. Nothing is written.
  uint32_t dpu = 0, scanout = 0;
  PE_parse_boot_argn ("dpu", &dpu, sizeof (dpu));
  PE_parse_boot_argn ("dpu-scanout", &scanout, sizeof (scanout));
  if (dpu >= 2 || scanout) {
    const uintptr_t s = gHost.smmu;
    const uint32_t idr0 = read32 (s, 0x20), idr1 = read32 (s, 0x24);
    const unsigned groups = (idr0 & 255) > 128 ? 128 : (idr0 & 255), banks = idr1 & 255;
    const unsigned pages = 1U << (((idr1 >> 28) & 7) + 1);
    native_ufs_dpu_smmu[1] = read32 (s, 0x0); native_ufs_dpu_smmu[2] = idr0; native_ufs_dpu_smmu[3] = idr1;
    unsigned valid = 0;
    for (unsigned k = 0; k < 2; ++k) native_ufs_dpu_smmu[4 + 9 * k] = 0xffffffffU;
    for (unsigned i = 0; i < groups; ++i) {
      const uint32_t smr = read32 (s, 0x800 + 4 * i);
      if (!(smr & 0x80000000U)) continue;
      ++valid;
      const uint32_t mask = (smr >> 16) & 0x7fff;
      for (unsigned k = 0; k < 2; ++k) {
        volatile uint32_t *r = &native_ufs_dpu_smmu[4 + 9 * k];
        if (r[0] != 0xffffffffU || ((smr ^ (0x1c00U + 2 * k)) & 0x7fff & ~mask)) continue;
        const uint32_t s2cr = read32 (s, 0xc00 + 4 * i), bank = s2cr & 255;
        r[0] = i | (bank << 16); r[1] = smr; r[2] = s2cr;
        if (((s2cr >> 16) & 3) == 0 && bank < banks && (pages + banks) * 4096 <= 0xd0000) {
          const uintptr_t cb = s + (pages + bank) * 4096;
          r[3] = read32 (s, 0x1000 + 4 * bank); r[4] = read32 (s, 0x1800 + 4 * bank);
          r[5] = read32 (cb, 0x0); r[6] = read32 (cb, 0x20); r[7] = read32 (cb, 0x24); r[8] = read32 (cb, 0x30);
          native_ufs_dpu_smmu[22 + k] = read32 (cb, 0x58);
        }
      }
    }
    native_ufs_dpu_smmu[0] = 0x80000000U | groups | (valid << 8) | (banks << 16);
    // dpu-scanout=1 or more: stage 1 for the display's bank (i562: bank 3, CBAR type 1, SCTLR 0xe0 = translation off, so the display
    // fetched physical addresses; its pipe address registers are 32-bit and every kernel allocation is above 4 GiB). The table keeps
    // 0x80000000..0xffffffff as it was (two 1 GiB identity blocks, so the boot loader's scanout at 0xb8000000 carries on unchanged) and adds
    // kDpuSlots windows of kDpuSlotBytes at kDpuWindow for the compositor's surfaces, filled by native_ufs_dpu_map. Undone at once when the
    // bank does not take the table or a check translation of 0xb8000000 does not come back identical.
    const uint32_t found = native_ufs_dpu_smmu[4], bank = found >> 16;
    if (scanout && found != 0xffffffffU && bank < banks && ((native_ufs_dpu_smmu[6] >> 16) & 3) == 0 && !(native_ufs_dpu_smmu[9] & 1) &&
        ((native_ufs_dpu_smmu[7] >> 16) & 3) == 1) {
      dpuEnable (s, s + (pages + bank) * 4096, bank, native_ufs_dpu_smmu[9]);
    } else if (scanout) {
      native_ufs_dpu_state = 0xdead0001U;
    }
  }
  uint32_t idr1 = read32 (gHost.smmu, 0x24);
  gHost.cb = gHost.smmu + ((1U << (((idr1 >> 28) & 7) + 1)) + (report[0] >> 16)) * 4096;
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.gfsrBefore = read32 (gHost.smmu, 0x48);
    gRootDiagnostic.fsrBefore = read32 (gHost.cb, 0x58);
    gRootDiagnostic.smmuId2 = read32 (gHost.smmu, 0x28);
    gRootDiagnostic.cba2r = read32 (gHost.smmu, 0x1800 + 4 * (report[0] >> 16));
  }
  {
    uint64_t limit = wide ? ~0ULL : 0x100000000ULL;
    const uint32_t idr2 = gHost.smmu ? read32 (gHost.smmu, 0x28) : 0;
    for (unsigned shift = 0; shift <= 8; shift += 4) {
      const unsigned bits = ufsSmmuAddressBits ((idr2 >> shift) & 15);
      const uint64_t width = bits ? (bits >= 64 ? ~0ULL : 1ULL << bits) : 0;
      if (width < limit) limit = width;
    }
    gDmaLimit = limit;
  }
  hostSlotsUpgrade (regionSlots, entries, wide);
  if (!hostStartQueue (why)) return false;
  gRootDiagnostic.stage = "NOP OUT";
  // NOP OUT proves the transport before touching the SCSI layer.
  if (hostNop () != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "NOP OUT";
    return false;
  }
  uint32_t power = 0;
  gRootDiagnostic.stage = "power mode query";
  if (hostQueryRead (0x03, 0x02, &power) != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "power mode query";
    return false;
  }
  gRootDiagnostic.power = power;
  if ((power & 0xff) != 0x11) {
    gRootDiagnostic.stage = "device wake";
    // START STOP UNIT, power condition ACTIVE, to the UFS Device well-known LU (as Linux resumes).
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0xd0, ssu, 6, false, 0, 500000, &scsi) != kIOReturnSuccess) {
      gHost.ready = false;
      *why = "wake (START STOP UNIT)";
      return false;
    }
    IOSleep (50);
  }
  gRootDiagnostic.stage = "LUN ready";
  for (unsigned attempt = 0; attempt < 20; attempt++) {
    uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0, tur, 6, false, 0, 200000, &scsi) == kIOReturnSuccess) {
      gHost.ready = true;
      hostEnableInterrupt ();
      return true;
    }
    if (gHost.poisoned) break;
    IOSleep (10);
  }
  gHost.ready = false;
  *why = "LUN 0 not ready";
  return false;
}
}

namespace {
static uint32_t be32w (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

#include "RootLayout.h"
#include "RootWindowRead.h"
#include "VerifiedTableCache.h"
#include "RootPublication.h"

}

// ---- Persistent Data store window (data-persist) ----------------------------------------------------------
// The only part of this driver that ever writes. The consumer (NativeRamBlock's overlay) asks for a window of the given
// size at the END of the userdata partition. Mode 1 (boot argument data-persist=1) only reads: it proves the window and
// lets the consumer inspect its header. Mode 2 additionally opens the write window. A window is refused unless it lies inside
// the partition, clear of the flashed root image plus a 4 MiB guard, and is at most a quarter of the partition.
namespace {
struct StoreGeometry {
  bool valid;
  uint64_t first, last, imageBytes, entriesLba;
  uint32_t headerCrc, entriesCrc, entryCount;
};
StoreGeometry gStoreGeometry;
uint64_t gStoreRegionFirst, gStoreRegionBlocks;
// The diagnostic log (log=1): always the last 32 MiB of userdata, so the Data store's place does not depend on logging.
constexpr uint64_t kLogReserveBlocks = 8192;
// The log's export region: the 256 MiB just below the log window (the render server's pipeline caches, kits/sheng BootLog.c). It is
// part of the log's write window, so the store ends below both.
constexpr uint64_t kExportBlocks = 65536;
uint64_t gLogRegionFirst, gLogRegionBlocks;
// The reserve at the end of userdata that the Data store stays clear of: the log and its export region sit inside it (log=1), the rest
// is unused. log-reserve=<MiB>, default 1024; 0 gives all the room to Data and is only accepted with the log off. The store's place
// depends on it, so an image keeps one value for its whole life (a different value moves the store: the old one is read as blank).
constexpr uint32_t kDefaultReserveMiB = 1024;
static uint64_t reserveBlocks (void)
{
  static int64_t cached = -1;
  if (cached < 0) {
    uint32_t mib = kDefaultReserveMiB;
    if (!PE_parse_boot_argn ("log-reserve", &mib, sizeof (mib)) || mib > 16384) mib = kDefaultReserveMiB;
    cached = static_cast<int64_t> (mib) * 256;   // 4096-byte blocks
  }
  return static_cast<uint64_t> (cached);
}

static unsigned storeMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("data-persist", &value, sizeof (value)) || value > 2) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}
}

extern "C" unsigned native_ufs_store_mode (void) { return storeMode (); }

// Returns 1 and the first absolute 4096-byte LBA of the window, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_store_open (uint64_t blocks, uint64_t *firstLba, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!storeMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gStoreRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    const uint64_t reserve = reserveBlocks ();
    if (!blocks || reserve >= partition || blocks > partition - reserve) reason = "size";
    else if (partition - blocks - reserve < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      // The reserve at the end of the partition belongs to the diagnostic log (its last 32 MiB, with the export region below them);
      // the store ends just below it.
      uint64_t first = gStoreGeometry.last + 1 - reserve - blocks;
      if (storeMode () == 2 && !hostStoreOpen (first, blocks)) reason = "window refused";
      else {
        gStoreRegionFirst = first;
        gStoreRegionBlocks = blocks;
        if (firstLba) *firstLba = first;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

// Store window commands, in mach_absolute_time ticks: count and time of reads, writes and cache flushes, and the longest single call.
extern "C" volatile uint64_t native_ufs_store_rd_ops = 0, native_ufs_store_rd_ticks = 0, native_ufs_store_wr_ops = 0,
                             native_ufs_store_wr_ticks = 0, native_ufs_store_sync_ops = 0, native_ufs_store_sync_ticks = 0,
                             native_ufs_store_max_ticks = 0;
// What the longest call was, for the log: 'r' read, 'w' write, 's' cache flush, and its blocks. Written without a lock right after the
// longest time, so under a race the pair may name a call of almost the same length.
static volatile uint32_t gStoreMaxKind = 0, gStoreMaxBlocks = 0;
static void storeAccount (volatile uint64_t *ops, volatile uint64_t *ticks, uint64_t t0, char kind, uint32_t blocks)
{
  const uint64_t spent = mach_absolute_time () - t0;
  __atomic_fetch_add (ops, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add (ticks, spent, __ATOMIC_RELAXED);
  uint64_t seen = native_ufs_store_max_ticks;
  while (spent > seen) {
    if (!__atomic_compare_exchange_n (&native_ufs_store_max_ticks, &seen, spent, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
    __atomic_store_n (&gStoreMaxKind, static_cast<uint32_t> (kind), __ATOMIC_RELAXED);
    __atomic_store_n (&gStoreMaxBlocks, blocks, __ATOMIC_RELAXED);
    break;
  }
}

extern "C" int native_ufs_store_read (uint64_t rel, uint32_t blocks, void *dst)
{
  if (!gStoreRegionBlocks || !dst || !blocks || rel >= gStoreRegionBlocks || blocks > gStoreRegionBlocks - rel) return 0;
  const uint64_t t0 = mach_absolute_time ();
  const uint32_t asked = blocks;
  int ok = 1;
  while (blocks) {
    uint32_t chunk = blocks > kXferMax / 4096 ? static_cast<uint32_t> (kXferMax / 4096) : blocks;
    if (hostRead4k (0, gStoreRegionFirst + rel, chunk, dst) != kIOReturnSuccess) { ok = 0; break; }
    dst = static_cast<uint8_t *> (dst) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  storeAccount (&native_ufs_store_rd_ops, &native_ufs_store_rd_ticks, t0, 'r', asked);
  return ok;
}

static int storeWrite (uint64_t rel, uint32_t blocks, const void *src, bool fua)
{
  if (storeMode () != 2 || !gStoreRegionBlocks || !src || !blocks || rel >= gStoreRegionBlocks ||
      blocks > gStoreRegionBlocks - rel) return 0;
  const uint64_t t0 = mach_absolute_time ();
  const uint32_t asked = blocks;
  int ok = 1;
  while (blocks) {
    uint32_t chunk = blocks > kXferMax / 4096 ? static_cast<uint32_t> (kXferMax / 4096) : blocks;
    if (hostWrite4k (0, gStoreRegionFirst + rel, chunk, src, fua) != kIOReturnSuccess) { ok = 0; break; }
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  storeAccount (&native_ufs_store_wr_ops, &native_ufs_store_wr_ticks, t0, 'w', asked);
  return ok;
}

// Forced unit access: the block is on the medium when the command completes, so a write-through overlay needs no cache flush.
extern "C" int native_ufs_store_write (uint64_t rel, uint32_t blocks, const void *src)
{
  return storeWrite (rel, blocks, src, true);
}

// No forced unit access: the device may keep the blocks in its volatile write cache until native_ufs_store_sync. For a
// write-back overlay, which makes its pages durable with a sync before it writes the bitmap blocks that name them.
extern "C" int native_ufs_store_write_cached (uint64_t rel, uint32_t blocks, const void *src)
{
  return storeWrite (rel, blocks, src, false);
}

extern "C" int native_ufs_store_sync (void)
{
  if (storeMode () != 2 || !gStoreRegionBlocks) return 0;
  const uint64_t t0 = mach_absolute_time ();
  const int ok = hostSyncCache () == kIOReturnSuccess;
  storeAccount (&native_ufs_store_sync_ops, &native_ufs_store_sync_ticks, t0, 's', 0);
  return ok;
}

// ---- Read-only access to other partitions, by GPT name ------------------------------------------------------------
// The firmware the board's own boot stages left on the modem partition is read from there (the audio DSP's images), so it never has to be
// part of the boot image. Nothing here writes: only reads of 4096-byte blocks of the LUN the name was found on.
// The stream-match slot | context bank << 16 of the extra pass-through stream, bit 31 set once it exists (0: not requested).
extern "C" volatile uint32_t native_ufs_extra_stream = 0;
// The endpoint's DMA buffer: physically contiguous, seen by the endpoint at native_ufs_extra_iova (it only addresses 32 bits).
extern "C" uint8_t *native_ufs_extra_va = nullptr;
extern "C" volatile uint32_t native_ufs_extra_uncached = 0;
extern "C" volatile uint32_t native_ufs_extra_iova = 0, native_ufs_extra_bytes = 0;
// The endpoint's MSI doorbell (qcom,pci-msi@0x17110040) reached through this bank: the real
// MSI delivery needs the endpoint to write the SoC's MSI bitmap, not a host memory page.
extern "C" volatile uint32_t native_ufs_msi_iova = 0;
extern "C" volatile uint32_t native_ufs_extra_atos[8] = { 0, 0, 0, 0 };
// The display streams' state as the boot loader left it (dpu=2): [0] bit 31 read, stream-match groups | valid ones << 8 | context banks << 16;
// [1] sCR0, [2] IDR0, [3] IDR1; for 0x1c00 at [4] and 0x1c02 at [13]: group | bank << 16 (all ones: no match), SMR, S2CR, CBAR, CBA2R, SCTLR,
// TTBR0 low, high, TCR; [22], [23] the two banks' FSR.
extern "C" volatile uint32_t native_ufs_dpu_smmu[24] = {};
// The display bank's stage 1 (dpu-scanout): bit 31 on, low bits the bank; 0xdead000n the step that refused it (1 bank not as expected,
// 2 no memory, 3 the bank ignored the table registers, 4 TLB sync timeout, 5 the display's fetches faulted with translation on).
extern "C" volatile uint32_t native_ufs_dpu_state = 0;
// TTBR0 low and TCR read back, FSR before and after the 50 ms watch, FAR low and high and FSYNR0 of the first fault, us watched.
extern "C" volatile uint32_t native_ufs_dpu_check[8] = {};
// The display bank's SCTLR (reg 0) or FSR (reg 1) now, for the log; all ones before the bank was touched.
extern "C" uint32_t native_ufs_dpu_live (unsigned reg)
{
  return gDpuLiveCb ? read32 (gDpuLiveCb, reg ? 0x58 : 0x0) : 0xffffffffU;
}

// Points slot (0 .. kDpuSlots-1) of the display's window at count 4 KiB pages and returns the slot's address for the display, 0 when
// translation is off or the pages do not fit. The caller makes sure the display is not fetching from the slot while it changes.
extern "C" uint32_t native_ufs_dpu_map (unsigned slot, const uint64_t *pages, unsigned count)
{
  if (!gDpuTables || slot >= kDpuSlots || count > kDpuSlotBytes / 4096) return 0;
  uint64_t *l3 = gDpuTables + 1024 + static_cast<size_t> (slot) * kDpuSlotTables * 512;
  const uint64_t attrs = (1ULL << 10) | (1ULL << 6);
  for (unsigned i = 0; i < kDpuSlotTables * 512; ++i) {
    if (i < count && (pages[i] & 0xfff)) return 0;
    l3[i] = i < count ? (pages[i] | attrs | 3) : 0;
  }
  dpuClean (l3, kDpuSlotTables * 4096);
  IOLockLock (gDpuLock);
  const bool synced = dpuTlbFlush (gDpuCb);
  IOLockUnlock (gDpuLock);
  return synced ? kDpuWindow + slot * kDpuSlotBytes : 0;
}

extern "C" int native_ufs_part_find (const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
{
  if (!gHost.ready || !name || !lun || !first || !blocks) return 0;
  uint8_t *buffer = static_cast<uint8_t *> (IOMalloc (4096));
  if (!buffer) return 0;
  int found = 0;
  const size_t length = strlen (name);
  for (uint32_t l = 0; l < 6 && !found; ++l) {
    if (hostRead4k (static_cast<uint8_t> (l), 1, 1, buffer) != kIOReturnSuccess || memcmp (buffer, "EFI PART", 8)) continue;
    uint64_t entriesLba;
    uint32_t count, size;
    memcpy (&entriesLba, buffer + 72, 8);
    memcpy (&count, buffer + 80, 4);
    memcpy (&size, buffer + 84, 4);
    if (size < 128 || size > 4096 || count > 512) continue;
    for (uint32_t i = 0; i < count && !found; ++i) {
      const uint64_t at = static_cast<uint64_t> (i) * size;
      if (i == 0 || at % 4096 < size) {
        if (hostRead4k (static_cast<uint8_t> (l), entriesLba + at / 4096, 1, buffer) != kIOReturnSuccess) break;
      }
      const uint8_t *entry = buffer + at % 4096;
      uint64_t firstLba, lastLba;
      memcpy (&firstLba, entry + 32, 8);
      memcpy (&lastLba, entry + 40, 8);
      if (!firstLba || lastLba < firstLba) continue;
      size_t n = 0;
      while (n < length && n < 36 && entry[56 + 2 * n] == static_cast<uint8_t> (name[n]) && entry[57 + 2 * n] == 0) ++n;
      if (n == length && (n == 36 || (entry[56 + 2 * n] == 0 && entry[57 + 2 * n] == 0))) {
        *lun = l;
        *first = firstLba;
        *blocks = lastLba - firstLba + 1;
        found = 1;
      }
    }
  }
  IOFree (buffer, 4096);
  return found;
}

// Reads `blocks` 4096-byte blocks from lba of the LUN (absolute, as native_ufs_part_find returned them).
extern "C" int native_ufs_part_read (uint32_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
  if (!gHost.ready || !dst || !blocks || lun > 5) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kXferMax / 4096 ? static_cast<uint32_t> (kXferMax / 4096) : blocks;
    if (hostRead4k (static_cast<uint8_t> (lun), lba, chunk, dst) != kIOReturnSuccess) return 0;
    dst = static_cast<uint8_t *> (dst) + chunk * 4096ULL;
    lba += chunk;
    blocks -= chunk;
  }
  return 1;
}

// The fault registers of the endpoint's stream: global fault status and syndrome, then the context bank's FSR, FAR (low, high) and FSYNR0.
extern "C" int native_ufs_extra_fault (uint32_t *out)
{
  if (!gExtraSmmu || !gExtraCb) return 0;
  out[0] = read32 (gExtraSmmu, 0x48); out[1] = read32 (gExtraSmmu, 0x50);
  out[2] = read32 (gExtraCb, 0x58); out[3] = read32 (gExtraCb, 0x60); out[4] = read32 (gExtraCb, 0x64); out[5] = read32 (gExtraCb, 0x68);
  out[6] = read32 (gExtraCb, 0x0); out[7] = read32 (gExtraCb, 0x30); out[8] = read32 (gExtraCb, 0x20); out[9] = read32 (gExtraSmmu, 0x1000 + 4 * gExtraBank);
  return 1;
}

// ---- Diagnostic log window (log=1) --------------------------------------------------------------------------
// The board's log writer (kits/sheng BootLog.c) gets the last kLogReserveBlocks of userdata and the kExportBlocks below them, refused
// unless the partition holds them clear of the flashed root image plus a 4 MiB guard. Off without the boot argument.
static unsigned logMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("log", &value, sizeof (value)) || value > 1) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}

// Returns 1 with the window's first absolute LBA and size in 4096-byte blocks, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_log_open (uint64_t *firstLba, uint64_t *blocks, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!logMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gLogRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    if (reserveBlocks () < kLogReserveBlocks + kExportBlocks) reason = "reserve too small for the log";
    else if (partition - reserveBlocks () < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      uint64_t first = gStoreGeometry.last + 1 - kLogReserveBlocks;
      // One write window covers the export region and the log: [first - kExportBlocks, first + kLogReserveBlocks).
      if (!hostLogOpen (first - kExportBlocks, kExportBlocks + kLogReserveBlocks)) reason = "window refused";
      else {
        gLogRegionFirst = first;
        gLogRegionBlocks = kLogReserveBlocks;
        if (firstLba) *firstLba = first;
        if (blocks) *blocks = kLogReserveBlocks;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_log_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= gLogRegionBlocks || blocks > gLogRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (0, gLogRegionFirst + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

// The export region below the log window: its size in blocks once the log is open, else 0; writes at a block offset inside it.
extern "C" uint64_t native_ufs_export_blocks (void) { return gLogRegionBlocks ? kExportBlocks : 0; }

extern "C" int native_ufs_export_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= kExportBlocks || blocks > kExportBlocks - rel) return 0;
  const uint64_t base = gLogRegionFirst - kExportBlocks;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (0, base + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

// ---- UniPro attributes (read only) --------------------------------------------------------------------------------
// DME_GET through the UIC command registers (UFSHCI 5.6): wait for HCS.UCRDY, write UCMDARG1..3 and UICCMD, poll IS.UCCS, read the
// result code (UCMDARG2[7:0]) and the value (UCMDARG3). Interrupts stay masked, so completion is polled. Under the slot pool lock: one
// UIC command at a time. Returns 1 with *value, 0 on refusal, timeout or a nonzero result code (*value then holds 0xffff0000 | code).
extern "C" int native_ufs_dme_get (uint32_t attribute, uint32_t selector, uint32_t *value)
{
  if (!gHost.ready || !value || gHostPanic) return 0;
  IOLockLock (gHost.lock);
  int ok = 0;
  *value = 0xffffffffU;
  if (hostWaitRegister (0x30, 1U << 3, 1U << 3)) {
    hostWrite (0x20, 1U << 10);                       // clear a stale UIC completion
    hostWrite (0x94, (attribute << 16) | (selector & 0xffff));
    hostWrite (0x98, 0);
    hostWrite (0x9c, 0);
    hostWrite (0x90, 0x01);                           // DME_GET
    if (hostWaitRegister (0x20, 1U << 10, 1U << 10)) {
      const uint32_t code = read32 (gHost.hc, 0x98) & 0xff;
      if (!code) { *value = read32 (gHost.hc, 0x9c); ok = 1; }
      else *value = 0xffff0000U | code;
      hostWrite (0x20, 1U << 10);
    }
  }
  IOLockUnlock (gHost.lock);
  return ok;
}

// Command slots: in use, the most taken at once, takes that waited for a free slot (RootCommands.h), for the log.
extern "C" void native_ufs_slot_stats (uint32_t *slots, uint32_t *peak, uint64_t *waits)
{
  *slots = gSlotCount; *peak = gSlotsPeak; *waits = gSlotWaits;
}

extern "C" int native_ufs_log_sync (void)
{
  return gLogRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// From a panic, before the log writes that keep its message: later host commands no longer wait on the host lock. 1 = the log is open.
extern "C" int native_ufs_log_panic (void)
{
  gHostPanic = true;
  return gLogRegionBlocks != 0;
}

// Re-read the GPT header and entry array and compare their CRCs with what was recorded at root start. 0 = unchanged.
extern "C" unsigned native_ufs_store_gpt_check (void)
{
  if (!gStoreGeometry.valid || !gHost.ready) return 1;
  static uint8_t block[4096];
  if (hostRead4k (0, 1, 1, block) != kIOReturnSuccess) return 2;
  uint32_t headerBytes = le32 (block + 12);
  if (headerBytes < 92 || headerBytes > 4096 || rootCrc32 (block, headerBytes, true) != le32 (block + 16) ||
      le32 (block + 16) != gStoreGeometry.headerCrc) return 3;
  uint32_t blocks = (gStoreGeometry.entryCount * 128 + 4095) / 4096;
  static uint8_t entries[4096 * 4];
  if (blocks > 4 || hostRead4k (0, gStoreGeometry.entriesLba, blocks, entries) != kIOReturnSuccess) return 4;
  return rootCrc32 (entries, gStoreGeometry.entryCount * 128) == gStoreGeometry.entriesCrc ? 0 : 5;
}

// Root window requests, for the log (native_ufs_stats_text): their count, bytes and time from entry to the completion call (the wait
// for a bounce buffer, the reads, the block verification and the copy to the caller), and the requests that had to wait for a bounce
// buffer with the time they waited.
static volatile uint64_t gWindowRequests = 0, gWindowBytes = 0, gWindowTicks = 0, gBounceWaits = 0, gBounceWaitTicks = 0;

// One read-only window of the root image as a block device: 512-byte sectors on top of 4096-byte
// UFS blocks, so the unchanged dmg disk images (GPT with 512-byte sectors) parse as they do on
// the virtio path.
class NativeUfsBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeUfsBlock)
  uint64_t absolute = 0;      // byte offset of the window on LUN 0
  uint64_t bytes = 0;
  char label[32] = {};
  // Requests run concurrently, each with its own bounce buffer from the pool: a request verifies and copies the snapshot in its
  // own buffer, so no lock is held across the read, the integrity check or the copy (RootWindowRead.h).
  static constexpr unsigned kBounces = 4;
  uint8_t *bounces[kBounces] = {};
  uint32_t bounceFree = 0;
  IOLock *windowLock = nullptr;   // guards bounceFree
  bool online = false;
  uint8_t *takeBounce ()
  {
    IOLockLock (windowLock);
    if (!bounceFree) {
      const uint64_t t0 = mach_absolute_time ();
      while (!bounceFree) IOLockSleep (windowLock, &bounceFree, THREAD_UNINT);
      __atomic_fetch_add (&gBounceWaits, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add (&gBounceWaitTicks, mach_absolute_time () - t0, __ATOMIC_RELAXED);
    }
    const unsigned k = static_cast<unsigned> (__builtin_ctz (bounceFree));
    bounceFree &= ~(1U << k);
    IOLockUnlock (windowLock);
    return bounces[k];
  }
  void giveBounce (uint8_t *buffer)
  {
    IOLockLock (windowLock);
    for (unsigned k = 0; k < kBounces; ++k) if (bounces[k] == buffer) bounceFree |= 1U << k;
    IOLockWakeup (windowLock, &bounceFree, true);
    IOLockUnlock (windowLock);
  }
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  struct vr_view verified = {};
  vm_offset_t verifiedStorage = 0;
  vm_size_t verifiedStorageBytes = 0;
  bool verifiedReady = false;          // the signed header (and its whole table) authenticated
  // Cached table mode (verified.hashes is null): page hashes and the cache index of a header-only policy.
  VrootTable table = {};
  vm_offset_t tableHashStorage = 0, tableIndexStorage = 0;
  vm_size_t tableHashBytes = 0, tableIndexBytes = 0;
  // SHA-384 of header, signature and table for the root authorization query, taken on its first use from a fresh read of the
  // table that is authenticated again against the signed header.
  uint8_t envelopeDigest[48] = {};
  volatile bool envelopeDigestReady = false;
  bool cachedEnvelopeDigest (uint8_t out[48]);
  bool configureVerification ();
  bool configureCachedTable (const uint8_t *manifest, uint64_t hashBytes);
#endif
public:
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  bool authenticatesVolume (const char *uuid, uint8_t *manifestDigest = nullptr);
  bool signedVolumeUuid (char out[37]) const;
#endif
  const char *windowLabel () const { return label; }
  bool configure (const char *name, uint64_t absoluteOffset, uint64_t size);
  bool start (IOService *provider) override;
  void free () override;
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("Native"); }
  char *getProductString () override { return label; }
  char *getRevisionString () override { return const_cast<char *> ("1.0"); }
  char *getAdditionalDeviceInfoString () override { return const_cast<char *> ("UFS LUN 0 userdata window, read-only"); }
  IOReturn reportBlockSize (UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override { *value = bytes / 512 - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache () override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors (NativeUfsBlock, IOBlockStorageDevice)

#if defined(NATIVE_UFS_SOURCE_API) && NATIVE_UFS_SOURCE_API
// The source runtime owns registration and attachment after IOStorageFamily
// has loaded. No second IOKit personality starts this service in that mode.
extern "C" IOService *native_ufs_source_create (void)
{
  return new NativeUfs;
}
#endif

extern "C" {
volatile uint32_t native_ufs_blocks_published = 0;
volatile uint64_t native_ufs_block_reads = 0;
volatile uint64_t native_ufs_block_failures = 0;
}

static char gRootDiagnosticText[2500];
static char gRootMediaText[1000];
// Passive diagnostic reader: cached software values only. It must remain safe
// when called from an IRQ while the transport is busy: no locks, allocations,
// MMIO reads, queue resets, or changes to the command/trust state. Fields can
// represent adjacent updates; this is explicitly not an atomic HW snapshot.
extern "C" void native_ufs_progress_text (char *out, size_t size)
{
  UfsRootDiagnostics sample = gRootDiagnostic;
  sample.published = native_ufs_blocks_published;
  sample.reads = native_ufs_block_reads;
  sample.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (out, size, sample, "cached values; not a new HW fault sample", "UFS CACHED SNAPSHOT");
}

static void rootDiagnosticFailure (const char *reason)
{
  // Kept even when the panic diagnostic is off, so the boot page can say why nothing was published.
  native_ufs_root_failure = reason;
  if (!gRootDiagnosticEnabled) return;
  gRootDiagnostic.published = native_ufs_blocks_published;
  gRootDiagnostic.reads = native_ufs_block_reads;
  gRootDiagnostic.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (gRootDiagnosticText, sizeof (gRootDiagnosticText), gRootDiagnostic, reason);
  strlcat (gRootDiagnosticText, gRootMediaText, sizeof (gRootDiagnosticText));
  panic ("%s", gRootDiagnosticText);
}

bool NativeUfsBlock::configure (const char *name, uint64_t absoluteOffset, uint64_t size)
{
  if (!name || !ufsWindowGeometry (absoluteOffset, size)) {
    return false;
  }
  strlcpy (label, name, sizeof (label));
  absolute = absoluteOffset;
  bytes = size;
  windowLock = IOLockAlloc ();
  if (!windowLock) return false;
  for (unsigned k = 0; k < kBounces; ++k) {
    bounces[k] = static_cast<uint8_t *> (IOMalloc (kDataMax));
    if (!bounces[k]) {
      if (!k) return false;
      break;
    }
    bounceFree |= 1U << k;
  }
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  // Reads are served only after each 4096-byte integrity block verifies against the owner-signed
  // policy; without a matching policy the device is never published.
  return configureVerification ();
#else
  return true;
#endif
}

// Data objects of the root image (version 2 manifest), recorded by startRoot before windows are configured.
static RootWindow gRootObject[4];
static uint32_t gRootObjects;
static uint64_t gRootObjectBase;

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
volatile uint64_t native_ufs_verified_blocks = 0;

// The LUN 0 block of the root image object "<window>.vroot", the hash table of a header-only policy, whose size must be tableBytes.
static bool policyTableLba (const char *window, uint64_t tableBytes, uint64_t *lba)
{
  char name[32];
  if (snprintf (name, sizeof (name), "%s.vroot", window) >= static_cast<int> (sizeof (name))) return false;
  const RootWindow *object = nullptr;
  for (unsigned i = 0; i < gRootObjects; ++i)
    if (!strcmp (gRootObject[i].name, name)) object = &gRootObject[i];
  if (!object || object->size != tableBytes || ((gRootObjectBase + object->offset) & 4095)) return false;
  *lba = (gRootObjectBase + object->offset) / 4096;
  return true;
}

// Read the hash table of a header-only policy into `table`.
// The bytes are untrusted here; vr_open checks them against the digest in the signed header.
static bool readPolicyTable (const char *window, uint8_t *table, uint64_t tableBytes)
{
  uint64_t lba = 0, done = 0;
  if (!policyTableLba (window, tableBytes, &lba)) return false;
  while (done < tableBytes) {
    uint64_t left = tableBytes - done;
    uint32_t blocks = static_cast<uint32_t> (left >= kDataMax ? kDataMax / 4096 : (left + 4095) / 4096);
    if (hostRead4k (0, lba, blocks, gScratch) != kIOReturnSuccess) return false;
    uint64_t take = left < blocks * 4096ULL ? left : blocks * 4096ULL;
    memcpy (table + done, gScratch, take);
    done += take;
    lba += blocks;
  }
  return true;
}
// Time spent hashing/verifying and reading from the UFS transport, in generic-timer ticks (diagnostic only).
volatile uint64_t native_ufs_verify_ticks = 0, native_ufs_read_ticks = 0;
static inline uint64_t timerTicks (void) {uint64_t v; __asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (v)); return v;}
volatile uint32_t native_ufs_integrity_failures = 0;

// Table pages of header-only policies are cached (VerifiedTableCache.h) unless native-vroot-cache=0, which keeps every
// window's whole table wired. native-vroot-cache=N sizes the cache shared by all windows in MiB (default 8, 2048 table pages,
// which cover 1 GiB of image). Each window's whole table is still read and authenticated at bring-up.
static VrootTableCache gVrootCache;
static IOLock *gVrootLock = nullptr;
static uint32_t gVrootCacheMiB = 8;
static bool gVrootCacheParsed = false;
volatile uint64_t native_ufs_vroot_wired_bytes = 0;   // wired bytes of policies, tables, page hashes, index and cache
struct VrootKernelOps {
  void lock () { IOLockLock (gVrootLock); }
  void unlock () { IOLockUnlock (gVrootLock); }
  void sleep (void *event) { IOLockSleep (gVrootLock, event, THREAD_UNINT); }
  void wakeup (void *event) { IOLockWakeup (gVrootLock, event, false); }
  // Straight to the transport: the table object is not a window, so this read never needs a verified read or an allocation.
  bool readPage (const VrootTable &table, uint64_t page, void *dst) {
    return hostRead4k (0, table.firstLba + page, 1, dst) == kIOReturnSuccess;
  }
  bool sha256 (const void *data, size_t bytes, uint8_t out[32]) { return vr_xnu_crypto.sha256 (data, bytes, out) != 0; }
};

static bool vrootCacheWanted ()
{
  if (!gVrootCacheParsed) {
    uint32_t mib = 0;
    if (PE_parse_boot_argn ("native-vroot-cache", &mib, sizeof (mib))) gVrootCacheMiB = mib;
    gVrootCacheParsed = true;
  }
  return gVrootCacheMiB != 0;
}

// Allocated once, before the first window is published, and kept: a fill never allocates.
static bool vrootCacheReady ()
{
  if (gVrootCache.count) return true;
  if (gVrootCacheMiB > UINT32_MAX / 256) return false;
  const uint32_t count = gVrootCacheMiB * 256;
  const vm_size_t pageBytes = static_cast<vm_size_t> (count) * 4096;
  const vm_size_t entryBytes = round_page (static_cast<vm_size_t> (count) * sizeof (VrootCacheEntry));
  vm_offset_t pages = 0, entries = 0;
  if (!gVrootLock && !(gVrootLock = IOLockAlloc ())) return false;
  if (kmem_alloc (kernel_map, &pages, pageBytes, static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA),
                  VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) return false;
  if (kmem_alloc (kernel_map, &entries, entryBytes, static_cast<kma_flags_t> (KMA_KOBJECT | KMA_ZERO),
                  VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) {
    kmem_free (kernel_map, pages, pageBytes);
    return false;
  }
  gVrootCache.pages = reinterpret_cast<uint8_t *> (pages);
  gVrootCache.entries = reinterpret_cast<VrootCacheEntry *> (entries);
  gVrootCache.count = count;
  __atomic_fetch_add (&native_ufs_vroot_wired_bytes, pageBytes + entryBytes, __ATOMIC_RELAXED);
  return true;
}

// Read the table object at `lba` in pieces through `buffer` (kDataMax bytes) and hash it as a whole into `whole` (the digest the
// signed header carries, as vr_open checks it). Optionally hash each 4096-byte page into `pageHashes` and the policy envelope
// (signed header `header`, then the table) into `envelope`. Every digest is taken from the same bytes of one read.
static bool hashPolicyTable (uint64_t lba, uint64_t hashBytes, uint8_t *buffer, uint8_t *pageHashes,
                             const uint8_t *header, uint8_t whole[32], uint8_t *envelope = nullptr)
{
  SHA256_CTX table;
  SHA384_CTX outer;
  SHA256_Init (&table);
  if (envelope) {
    SHA384_Init (&outer);
    SHA384_Update (&outer, header, VR_HEADER_BYTES + VR_SIGNATURE_BYTES);
  }
  for (uint64_t done = 0, page = 0; done < hashBytes;) {
    const uint64_t left = hashBytes - done;
    const uint32_t blocks = static_cast<uint32_t> (left >= kDataMax ? kDataMax / 4096 : (left + 4095) / 4096);
    if (hostRead4k (0, lba, blocks, buffer) != kIOReturnSuccess) return false;
    const uint64_t take = left < blocks * 4096ULL ? left : blocks * 4096ULL;
    SHA256_Update (&table, buffer, take);
    if (envelope) SHA384_Update (&outer, buffer, take);
    for (uint64_t at = 0; pageHashes && at < take; at += 4096, ++page) {
      const uint64_t length = take - at < 4096 ? take - at : 4096;
      if (!vr_xnu_crypto.sha256 (buffer + at, static_cast<size_t> (length), pageHashes + page * 32)) return false;
    }
    done += take;
    lba += blocks;
  }
  SHA256_Final (whole, &table);
  if (envelope) SHA384_Final (envelope, &outer);
  return true;
}

// A header-only policy without its whole table in memory: the signed header is authenticated and the table object is read
// once, hashed as a whole and page by page; a page hash is kept only if the whole table matches the signed digest.
bool NativeUfsBlock::configureCachedTable (const uint8_t *manifest, uint64_t hashBytes)
{
  constexpr size_t kSigned = VR_HEADER_BYTES + VR_SIGNATURE_BYTES;
  static_assert (kSigned + VR_KEY_BYTES <= 4096, "header, signature and key share one page");
  gRootDiagnostic.stage = "policy allocation";
  verifiedStorageBytes = round_page (static_cast<vm_size_t> (4096));
  if (kmem_alloc (kernel_map, &verifiedStorage, verifiedStorageBytes,
                  static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA | KMA_ZERO), VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) {
    verifiedStorage = 0;
    return false;
  }
  __atomic_fetch_add (&native_ufs_vroot_wired_bytes, verifiedStorageBytes, __ATOMIC_RELAXED);
  uint8_t *header = reinterpret_cast<uint8_t *> (verifiedStorage);
  uint8_t *key = header + 4096 - VR_KEY_BYTES;
  memcpy (header, manifest, kSigned);
  memcpy (key, verified_root_key, VR_KEY_BYTES);
  gRootDiagnostic.stage = "policy read-only protection";
  if (mach_vm_protect (kernel_map, verifiedStorage, verifiedStorageBytes, TRUE, VM_PROT_READ) != KERN_SUCCESS) return false;
  gRootDiagnostic.stage = "owner signature";
  int status = vr_open_header (header, kSigned, key, 1, &vr_xnu_crypto, &verified);
  gRootDiagnostic.verify = static_cast<uint32_t> (status);
  if (status != VR_OK || verified.image_bytes != bytes || verified.block_count * 32 != hashBytes) return false;

  gRootDiagnostic.stage = "policy table object";
  uint64_t lba = 0;
  if (!policyTableLba (label, hashBytes, &lba)) return false;
  const uint64_t pages = (hashBytes + 4095) / 4096;
  tableHashBytes = round_page (static_cast<vm_size_t> (pages * 32));
  tableIndexBytes = round_page (static_cast<vm_size_t> (pages * sizeof (uint32_t)));
  gRootDiagnostic.stage = "policy page hash allocation";
  if (kmem_alloc (kernel_map, &tableHashStorage, tableHashBytes, static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA),
                  VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) {
    tableHashStorage = 0;
    return false;
  }
  __atomic_fetch_add (&native_ufs_vroot_wired_bytes, tableHashBytes, __ATOMIC_RELAXED);
  if (kmem_alloc (kernel_map, &tableIndexStorage, tableIndexBytes, static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA | KMA_ZERO),
                  VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) {
    tableIndexStorage = 0;
    return false;
  }
  __atomic_fetch_add (&native_ufs_vroot_wired_bytes, tableIndexBytes, __ATOMIC_RELAXED);

  gRootDiagnostic.stage = "policy table object read";
  uint8_t *pageHashes = reinterpret_cast<uint8_t *> (tableHashStorage);
  uint8_t digest[32];
  if (!hashPolicyTable (lba, hashBytes, gScratch, pageHashes, nullptr, digest)) return false;
  gRootDiagnostic.stage = "owner signature and hash table";
  if (!vrootSame (digest, header + 88, 32)) {
    gRootDiagnostic.verify = VR_HASH_TABLE;
    return false;
  }
  gRootDiagnostic.stage = "policy read-only protection";
  if (mach_vm_protect (kernel_map, tableHashStorage, tableHashBytes, TRUE, VM_PROT_READ) != KERN_SUCCESS) return false;
  gRootDiagnostic.stage = "policy table cache allocation";
  if (!vrootCacheReady ()) return false;
  table.pageHashes = pageHashes;
  table.where = reinterpret_cast<uint32_t *> (tableIndexStorage);
  table.tableBytes = hashBytes;
  table.pages = pages;
  table.firstLba = lba;
  verifiedReady = true;
  return true;
}

bool NativeUfsBlock::configureVerification ()
{
  gRootDiagnostic.stage = "policy selection";
  gRootDiagnostic.windowBytes = bytes;
  gRootDiagnostic.policyCount = verified_root_policy_count;
  gRootDiagnostic.verify = 0xffffffff;
  // Select the one policy whose signed image size equals this window; two matches are refused.
  const uint8_t *manifest = nullptr;
  uint64_t manifestBytes = 0;
  if (!verified_root_policy_count || verified_root_policy_count > 16) {
    gRootDiagnostic.stage = "policy count invalid";
    return false;
  }
  for (uint64_t index = 0; index < verified_root_policy_count; ++index) {
    const NativeVerifiedBlockPolicy &policy = verified_root_policies[index];
    if (!policy.manifest || policy.manifestBytes < VR_HEADER_BYTES + VR_SIGNATURE_BYTES) {
      gRootDiagnostic.stage = "policy envelope invalid";
      return false;
    }
    uint64_t imageBytes = 0;
    for (unsigned i = 0; i < 8; ++i) {
      imageBytes |= static_cast<uint64_t> (policy.manifest[24 + i]) << (8 * i);
    }
    if (imageBytes != bytes) {
      continue;
    }
    if (manifest) {
      gRootDiagnostic.stage = "duplicate capacity-matching policy";
      return false;
    }
    manifest = policy.manifest;
    manifestBytes = policy.manifestBytes;
  }
  if (!manifest) {
    gRootDiagnostic.stage = "no capacity-matching policy";
    return false;
  }
  // A header-only policy carries its hash table in the root image instead of the boot image.
  const bool headerOnly = manifestBytes == VR_HEADER_BYTES + VR_SIGNATURE_BYTES;
  uint64_t hashBytes = 0;
  if (headerOnly) {
    uint64_t count = 0;
    for (unsigned i = 0; i < 8; ++i) count |= static_cast<uint64_t> (manifest[32 + i]) << (8 * i);
    if (!count || count > (UINT64_MAX - manifestBytes) / 32) {
      gRootDiagnostic.stage = "policy header block count";
      return false;
    }
    hashBytes = count * 32;
    if (vrootCacheWanted ()) return configureCachedTable (manifest, hashBytes);
    manifestBytes += hashBytes;
  }
  gRootDiagnostic.stage = "policy storage";
  vm_size_t tableBytes = round_page (static_cast<vm_size_t> (manifestBytes));
  if (tableBytes < manifestBytes || tableBytes > UINT64_MAX - 4096) {
    gRootDiagnostic.stage = "policy size overflow";
    return false;
  }
  verifiedStorageBytes = tableBytes + 4096;
  gRootDiagnostic.stage = "policy allocation";
  if (kmem_alloc (kernel_map, &verifiedStorage, verifiedStorageBytes,
                  static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA), VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) {
    verifiedStorage = 0;
    return false;
  }
  __atomic_fetch_add (&native_ufs_vroot_wired_bytes, verifiedStorageBytes, __ATOMIC_RELAXED);
  if (headerOnly) {
    memcpy (reinterpret_cast<void *> (verifiedStorage), manifest, VR_HEADER_BYTES + VR_SIGNATURE_BYTES);
    gRootDiagnostic.stage = "policy table object read";
    if (!readPolicyTable (label, reinterpret_cast<uint8_t *> (verifiedStorage) + VR_HEADER_BYTES + VR_SIGNATURE_BYTES,
                          hashBytes)) {
      return false;
    }
  } else {
    memcpy (reinterpret_cast<void *> (verifiedStorage), manifest, manifestBytes);
  }
  uint8_t *key = reinterpret_cast<uint8_t *> (verifiedStorage + tableBytes);
  memcpy (key, verified_root_key, VR_KEY_BYTES);
  gRootDiagnostic.stage = "policy read-only protection";
  if (mach_vm_protect (kernel_map, verifiedStorage, verifiedStorageBytes,
                       TRUE, VM_PROT_READ) != KERN_SUCCESS) {
    return false;
  }
  gRootDiagnostic.stage = "owner signature and hash table";
  int status = vr_open (reinterpret_cast<const uint8_t *> (verifiedStorage), manifestBytes, key, 1,
                        &vr_xnu_crypto, &verified);
  gRootDiagnostic.verify = static_cast<uint32_t> (status);
  verifiedReady = status == VR_OK && verified.hashes && verified.image_bytes == bytes;
  return verifiedReady;
}

bool NativeUfsBlock::cachedEnvelopeDigest (uint8_t out[48])
{
  if (!envelopeDigestReady) {
    // Thread context (the root mount); the buffer is the query's own, not the bring-up scratch.
    uint8_t *buffer = static_cast<uint8_t *> (IOMallocData (kDataMax));
    if (!buffer) return false;
    uint8_t whole[32], envelope[48];
    const uint8_t *header = reinterpret_cast<const uint8_t *> (verifiedStorage);
    const bool ok = hashPolicyTable (table.firstLba, table.tableBytes, buffer, nullptr, header, whole, envelope) &&
                    vrootSame (whole, header + 88, 32);
    IOFreeData (buffer, kDataMax);
    if (!ok) return false;
    memcpy (envelopeDigest, envelope, sizeof (envelope));
    __atomic_store_n (&envelopeDigestReady, true, __ATOMIC_RELEASE);
  }
  memcpy (out, envelopeDigest, sizeof (envelopeDigest));
  return true;
}

bool NativeUfsBlock::authenticatesVolume (const char *uuid, uint8_t *manifestDigest)
{
  if (!uuid || !online || !verifiedReady) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  char expected[37];
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      expected[at++] = '-';
    }
    expected[at++] = hex[verified.volume_uuid[i] >> 4];
    expected[at++] = hex[verified.volume_uuid[i] & 15];
  }
  expected[at] = 0;
  if (strcmp (uuid, expected)) {
    return false;
  }
  if (manifestDigest && !verified.hashes) {
    if (!cachedEnvelopeDigest (manifestDigest)) return false;
  } else if (manifestDigest) {
    // Hash only the authenticated envelope, excluding allocation padding and the key.
    const uint64_t envelope = VR_HEADER_BYTES + VR_SIGNATURE_BYTES + verified.block_count * 32;
    SHA384_CTX context;
    SHA384_Init (&context);
    SHA384_Update (&context, reinterpret_cast<const void *> (verifiedStorage), envelope);
    SHA384_Final (manifestDigest, &context);
  }
  return true;
}

bool NativeUfsBlock::signedVolumeUuid (char out[37]) const
{
  if (!verifiedReady) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out[at++] = '-';
    }
    out[at++] = hex[verified.volume_uuid[i] >> 4];
    out[at++] = hex[verified.volume_uuid[i] & 15];
  }
  out[at] = 0;
  return true;
}

// Called from the root authorization query in NativeVirtioBlock: -1 when `provider` is not a
// UFS window, otherwise 1 if its signed volume UUID equals `uuid`, else 0.
extern "C" int native_ufs_block_authenticates (IOService *provider, const char *uuid, uint8_t *digest)
{
  NativeUfsBlock *block = OSDynamicCast (NativeUfsBlock, provider);
  if (!block) {
    return -1;
  }
  return block->authenticatesVolume (uuid, digest) ? 1 : 0;
}
#endif

bool NativeUfsBlock::start (IOService *provider)
{
  if (!bounceFree || !windowLock || !IOBlockStorageDevice::start (provider)) {
    return false;
  }
  // The medium is flash. IOMediaBSDClient answers DKIOCISSOLIDSTATE from this dictionary, found from any media above this device;
  // without it XNU treats the disk as rotating (throttle periods, read-ahead and page-in cluster sizes, the mount's MNTK_SSD).
  OSDictionary *characteristics = OSDictionary::withCapacity (1);
  OSString *medium = OSString::withCString (kIOPropertyMediumTypeSolidStateKey);
  if (characteristics && medium && characteristics->setObject (kIOPropertyMediumTypeKey, medium)) {
    setProperty (kIOPropertyDeviceCharacteristicsKey, characteristics);
  }
  OSSafeReleaseNULL (medium);
  OSSafeReleaseNULL (characteristics);
  online = true;
  registerService ();
  return true;
}

IOReturn NativeUfsBlock::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                           IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= bytes / 512 ||
      count > bytes / 512 - block || buffer->getLength () < count * 512) {
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection () != kIODirectionIn) {
    return kIOReturnNotWritable;
  }
  if (!online) {
    return kIOReturnNotReady;
  }
  struct Operations {
    IOMemoryDescriptor *buffer;
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
    const vr_view *view;
    const VrootTable *table;   // cached table mode when view->hashes is null
#endif
    void lock () {}     // the request's own bounce buffer holds its snapshot
    void unlock () {}
    IOReturn read (uint64_t lba, uint32_t blocks, void *out) {
      uint64_t t0 = timerTicks ();
      IOReturn r = hostRead4k (0, lba, blocks, out);
      __atomic_fetch_add (&native_ufs_read_ticks, timerTicks () - t0, __ATOMIC_RELAXED);
      return r;
    }
    IOReturn verify (uint64_t index, const void *data, size_t length) {
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      uint64_t t0 = timerTicks ();
      int status;
      if (view->hashes) {
        status = vr_verify_block (view, index, data, length, &vr_xnu_crypto);
      } else {
        VrootKernelOps ops;
        status = vrootVerifyBlock (gVrootCache, ops, *table, *view, index, data, length, &vr_xnu_crypto);
      }
      __atomic_fetch_add (&native_ufs_verify_ticks, timerTicks () - t0, __ATOMIC_RELAXED);
      if (status != VR_OK) {
        gRootDiagnostic.stage = "read block integrity";
        gRootDiagnostic.verify = static_cast<uint32_t> (status);
        __atomic_fetch_add (&native_ufs_integrity_failures, 1, __ATOMIC_RELAXED);
        return kIOReturnNotPrivileged;
      }
      __atomic_fetch_add (&native_ufs_verified_blocks, 1, __ATOMIC_RELAXED);
#if defined(NATIVE_UFS_SOURCE_API) && NATIVE_UFS_SOURCE_API
      boot_progress_poll ();
#else
      if (boot_progress_poll) boot_progress_poll ();
#endif
#else
      (void) index; (void) data; (void) length;
#endif
      return kIOReturnSuccess;
    }
    uint64_t copy (uint64_t offset, const void *data, uint64_t length) {
      uint64_t copied = buffer->writeBytes (offset, data, length);
      if (copied == length) __atomic_fetch_add (&native_ufs_block_reads, 1, __ATOMIC_RELAXED);
      return copied;
    }
  } operations = { buffer
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
    , &verified, &table
#endif
  };
  const uint64_t entered = mach_absolute_time ();
  uint8_t *bounce = takeBounce ();
  UfsWindowResult outcome = ufsReadWindow (operations, absolute, bytes, block, count, bounce);
  giveBounce (bounce);
  __atomic_fetch_add (&gWindowRequests, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add (&gWindowBytes, outcome.done, __ATOMIC_RELAXED);
  __atomic_fetch_add (&gWindowTicks, mach_absolute_time () - entered, __ATOMIC_RELAXED);
  IOReturn result = outcome.status;
  uint64_t done = outcome.done;
  if (result != kIOReturnSuccess) {
    __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
    gRootDiagnostic.result = static_cast<uint32_t> (result);
    rootDiagnosticFailure ("root window read failed");
  }
  completion->action (completion->target, completion->parameter, result, done);
  return kIOReturnSuccess;
}

// One row of command and wait statistics for the board's log. It only reads counters, so any context may call it.
//   direct, staged    data commands straight to or from the caller's pages, and through the slot's data area with a copy
//   busy              commands the device answered with BUSY or TASK SET FULL and the driver sent again
//   long k/n          the longest store call (native_ufs_store_max_ticks): r read, w write, s cache flush, and its blocks
//   pauses n/us       IODelay (2) poll pauses of the completion waits and their total time (2 us each where the delay spins)
//   spins n/us        spinning poll pauses (one WFE each) and their total time
//   yields            spins that ended in a sleep because a preemption was pending on the CPU
//   done a/b/c/d/e    waits whose command completed under 50, 100, 200, 400 us after the wait began, and later
//   old ms/op/slot    the oldest command in flight now: its age, opcode (hex) and slot; old=0 when none is out
//   takers            threads waiting for a free command slot now
//   poison p/r/op     1 once a command timed out or the controller reported a fatal error (every later command is refused), with
//                     that command's IOReturn and opcode (hex)
//   irqs, sleeps      completion interrupts taken, and waits that slept for one
//   wakes n/us max    sleeps the interrupt ended, the total and the longest time from the interrupt to the waiter running again
//   timeouts          sleeps that ended on their 1 ms timeout instead
//   win n/MiB/ms      root window requests, their bytes and their time (bounce wait, reads, block verification, copy)
//   wait n/ms         root window requests that waited for a bounce buffer, and the time they waited
//   vb                integrity blocks verified
//   vt h/m/r/e/w/f    verified-root table cache: hits, misses, table page reads, evictions, waits for a page or entry being
//                     filled, refused table pages (read error or hash mismatch); all 0 with native-vroot-cache=0
//   vtc n/KiB         table cache pages (0 when every whole table is wired) and wired verified-root bytes (policies, tables or
//                     page hashes and index, cache) in KiB
extern "C" void native_ufs_stats_text (char *out, size_t size)
{
  if (!out || !size) return;
  const auto us = [] (uint64_t ticks) { uint64_t ns = 0; absolutetime_to_nanoseconds (ticks, &ns); return static_cast<unsigned long long> (ns / 1000); };
  const auto count = [] (const volatile uint64_t &value) { return static_cast<unsigned long long> (__atomic_load_n (&value, __ATOMIC_RELAXED)); };
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  const unsigned long long verified = count (native_ufs_verified_blocks);
  const VrootCacheStats &vt = gVrootCache.stats;
  const unsigned long long vtHits = count (vt.hits), vtMisses = count (vt.misses), vtReads = count (vt.reads),
                           vtEvictions = count (vt.evictions), vtWaits = count (vt.waits), vtFailures = count (vt.failures),
                           vtPages = gVrootCache.count, vtWiredKiB = count (native_ufs_vroot_wired_bytes) >> 10;
#else
  const unsigned long long verified = 0, vtHits = 0, vtMisses = 0, vtReads = 0, vtEvictions = 0, vtWaits = 0, vtFailures = 0,
                           vtPages = 0, vtWiredKiB = 0;
#endif
  const uint32_t kind = __atomic_load_n (&gStoreMaxKind, __ATOMIC_RELAXED);
  uint64_t spins = 0, spinTicks = 0, oldest = 0;
  unsigned oldSlot = 0;
  for (unsigned slot = 0; slot < kSlots; ++slot) { spins += gSlotSpin[slot].pauses; spinTicks += gSlotSpin[slot].ticks; }
  const uint64_t now = mach_absolute_time ();
  for (unsigned slot = 0; slot < kSlots; ++slot) {
    const uint64_t at = __atomic_load_n (&gSlotIssuedAt[slot], __ATOMIC_RELAXED);
    if (at && at < now && now - at > oldest) { oldest = now - at; oldSlot = slot; }
  }
  snprintf (out, size, "UFS direct=%llu staged=%llu busy=%llu long=%c/%u pauses=%llu/%lluus spins=%llu/%lluus yields=%llu "
            "done=%llu/%llu/%llu/%llu/%llu old=%llu/%x/%u takers=%u poison=%u/%x/%x irqs=%llu sleeps=%llu wakes=%llu/%lluus "
            "max=%lluus timeouts=%llu win=%llu/%lluM/%llums wait=%llu/%llums vb=%llu vt=%llu/%llu/%llu/%llu/%llu/%llu vtc=%llu/%llu",
            count (gDirectCommands), count (gStagedCommands), count (gBusyRetries), kind ? static_cast<char> (kind) : '-',
            __atomic_load_n (&gStoreMaxBlocks, __ATOMIC_RELAXED), count (gWaitPauses), us (count (gWaitPauseTicks)),
            static_cast<unsigned long long> (spins), us (spinTicks), count (gWaitYields), count (gWaitRange[0]), count (gWaitRange[1]),
            count (gWaitRange[2]), count (gWaitRange[3]), count (gWaitRange[4]), us (oldest) / 1000,
            oldest ? gSlotOpcode[oldSlot] : 0U, oldest ? oldSlot : 0U, gSlotWaitersNow, gHost.poisoned ? 1U : 0U,
            gHost.poisoned ? gRootDiagnostic.result : 0U, gHost.poisoned ? gRootDiagnostic.opcode : 0U,
            count (native_ufs_irqs), count (native_ufs_irq_sleeps), count (gIrqWakes), us (count (gIrqWakeTicks)),
            us (count (gIrqWakeMaxTicks)), count (gIrqTimeouts), count (gWindowRequests), count (gWindowBytes) >> 20,
            us (count (gWindowTicks)) / 1000, count (gBounceWaits), us (count (gBounceWaitTicks)) / 1000, verified,
            vtHits, vtMisses, vtReads, vtEvictions, vtWaits, vtFailures, vtPages, vtWiredKiB);
}

void NativeUfsBlock::free ()
{
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  if (table.where) {
    VrootKernelOps ops;
    vrootTableDrop (gVrootCache, ops, table);
    table = {};
  }
  if (tableHashStorage) {
    kmem_free (kernel_map, tableHashStorage, tableHashBytes);
    __atomic_fetch_sub (&native_ufs_vroot_wired_bytes, tableHashBytes, __ATOMIC_RELAXED);
    tableHashStorage = 0;
  }
  if (tableIndexStorage) {
    kmem_free (kernel_map, tableIndexStorage, tableIndexBytes);
    __atomic_fetch_sub (&native_ufs_vroot_wired_bytes, tableIndexBytes, __ATOMIC_RELAXED);
    tableIndexStorage = 0;
  }
  if (verifiedStorage) {
    kmem_free (kernel_map, verifiedStorage, verifiedStorageBytes);
    __atomic_fetch_sub (&native_ufs_vroot_wired_bytes, verifiedStorageBytes, __ATOMIC_RELAXED);
    verifiedStorage = 0;
  }
  verifiedReady = false;
#endif
  for (unsigned k = 0; k < kBounces; ++k) {
    if (bounces[k]) IOFree (bounces[k], kDataMax);
    bounces[k] = nullptr;
  }
  bounceFree = 0;
  if (windowLock) {
    IOLockFree (windowLock);
    windowLock = nullptr;
  }
  IOBlockStorageDevice::free ();
}

// Ordered publication of the root windows. The original owner-trust code resolves the SystemOS
// volume by UUID immediately after the root is mounted, so SystemOS must already have published its
// volume media by then; the OS window (whose volume is the root being waited for) goes last.
static NativeUfsBlock *gPendingDevice[4];
static unsigned gPending;
static bool gPendingAttached[4];
static char gPendingUuid[4][37];
static UfsRootPlan gPublicationPlan;
static NativeUfs *gPendingProvider;

static bool mediaWithUuidExists (const char *uuid, NativeUfsBlock *window)
{
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity (1);
  OSString *wanted = OSString::withCString (uuid);
  bool found = false;
  if (matching && properties && wanted && properties->setObject ("UUID", wanted) &&
      matching->setObject ("IOPropertyMatch", properties)) {
    OSIterator *iterator = IOService::getMatchingServices (matching);
    if (iterator) {
      for (unsigned matches = 0; matches < 64 && !found; ++matches) {
        IORegistryEntry *media = OSDynamicCast (IORegistryEntry, iterator->getNextObject ());
        if (!media) break;
        IORegistryIterator *parents = IORegistryIterator::iterateOver (
          media, gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
        if (parents) {
          found = ufsMediaHasAncestor (*parents, window);
          parents->release ();
        }
      }
      iterator->release ();
    }
  }
  OSSafeReleaseNULL (wanted);
  OSSafeReleaseNULL (properties);
  OSSafeReleaseNULL (matching);
  return found;
}

// What sits below the window in the service plane, matched or not (getMatchingServices lists only services whose matching has
// finished, so media held up in matching would not show): each entry's class, BSD name, busy count and whether its matching is done.
static void captureMissingMedia (const char *uuid, IOService *window)
{
  int used = snprintf (gRootMediaText, sizeof (gRootMediaText), "Waiting for signed UUID %s\n", uuid);
  IORegistryIterator *children = window ? IORegistryIterator::iterateOver (window, gIOServicePlane, kIORegistryIterateRecursively) : nullptr;
  unsigned count = 0;
  while (children && used < int (sizeof (gRootMediaText)) - 160 && count++ < 8) {
    IOService *entry = OSDynamicCast (IOService, children->getNextObject ());
    if (!entry) break;
    OSString *id = OSDynamicCast (OSString, entry->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, entry->getProperty (kIOBSDNameKey));
    used += snprintf (gRootMediaText + used, sizeof (gRootMediaText) - used, "%.32s %.12s busy=%u matched=%u uuid=%.36s\n",
                      entry->getMetaClass ()->getClassName (), bsd ? bsd->getCStringNoCopy () : "-", entry->getBusyState (),
                      entry->isInactive () ? 2u : (entry->getState () & kIOServiceMatchedState) ? 1u : 0u,
                      id ? id->getCStringNoCopy () : "-");
  }
  OSSafeReleaseNULL (children);
}

// Used only for objects that were never started/published. Started objects
// retain their owner references even on a later failure; clients can have I/O.
static void discardPendingWindow (unsigned index)
{
  NativeUfsBlock *device = gPendingDevice[index];
  if (!device) return;
  if (gPendingAttached[index]) device->detach (gPendingProvider);
  gPendingAttached[index] = false;
  gPendingDevice[index] = nullptr;
  device->release ();
}

static void publishThread (void *, wait_result_t)
{
  struct Operations {
    bool start (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      gRootDiagnostic.stage = "root window service start";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Start);
      gRootDiagnostic.publicationWindow = index;
      if (!device->start (gPendingProvider)) return false;
      native_ufs_blocks_published = native_ufs_blocks_published + 1;
      IOLog ("NativeUfs: published %s\n", device->windowLabel ());
      return true;
    }
    bool wait (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      const char *uuid = gPendingUuid[index];
      gRootDiagnostic.stage = "media publication";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Media);
      gRootDiagnostic.publicationWindow = index;
      bool ready = false;
      // Diagnostic mode affects reporting only, never the dependency gate.
      for (unsigned tick = 0; tick <= 300; ++tick) {
        ready = mediaWithUuidExists (uuid, device);
        if (ready || tick == 300) break;
        if (tick && tick % 50 == 0) {
          // Every 5 s: what the IOKit side has published so far, and whether the window's own matching is still busy.
          captureMissingMedia (uuid, device);
          IOLog ("NativeUfs: %s volume %s still missing after %u s, window busy %u\n%s", device->windowLabel (), uuid,
                 tick / 10, device->getBusyState (), gRootMediaText);
        }
        IOSleep (100);
      }
      IOLog ("NativeUfs: %s volume %s %s\n", device->windowLabel (), uuid, ready ? "ready" : "not ready");
      if (!ready) {
        captureMissingMedia (uuid, device);
        IOLog ("NativeUfs: media at timeout, window busy %u\n%s", device->getBusyState (), gRootMediaText);
      }
      return ready;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations;
  UfsRootPublicationResult result = ufsPublishRootWindows (operations, gPublicationPlan);
  if (result.phase != UfsRootPhase::Complete) {
    gRootDiagnostic.result = result.phase == UfsRootPhase::Start ? kIOReturnIOError : kIOReturnTimeout;
    const char *reason = result.phase == UfsRootPhase::Start ? "root window service start failed" :
      (!strcmp (gPendingDevice[result.index]->windowLabel (), "SystemOS") ?
       "SystemOS signed volume not found" : "signed root window volume not found");
    IOLog ("NativeUfs: root publisher stopped at window %u; published=%u\n", result.index, result.published);
    // Start failures have already released their unpublished object; do not
    // dereference it here. The media-failure object is deliberately retained.
    rootDiagnosticFailure (reason);
  }
  gPendingProvider->release ();  // Balanced with successful thread dispatch.
}

// Memory snapshot (native-ufs-probe=13): root mode as usual, then, after a delay, the kernel's own
// page counters on the panic screen. Observation only.
extern "C" {
extern unsigned int vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
  vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count;
extern uint32_t vm_page_pages;
extern unsigned int memorystatus_available_pages;
extern int nprocs;
}

static void statsThread (void *argument, wait_result_t)
{
  unsigned delay = static_cast<unsigned> (reinterpret_cast<uintptr_t> (argument));
  IOSleep (delay * 1000);
  panic ("NATIVE_UFS memory snapshot after %u s\n"
         "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
         "available=%u max_mem=%llx procs=%d blocks=%llu\n",
         delay, vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
         vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
         memorystatus_available_pages, max_mem, nprocs, native_ufs_block_reads);
}

// Storage discovery report (native-ufs-probe=12): a separate thread waits for the original
// IOStorageFamily to publish IOMedia for the root windows, then shows what exists on the screen.
// It must not run inside the matching thread that is starting NativeUfs.
static char gMediaReport[3000];

static void mediaReportThread (void *, wait_result_t)
{
  unsigned count = 0;
  for (unsigned tick = 0; tick < 200; tick++) {      // up to 20 s
    OSDictionary *matching = IOService::serviceMatching ("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
    count = 0;
    if (iterator) {
      while (iterator->getNextObject ()) {
        count++;
      }
      iterator->release ();
    }
    if (matching) {
      matching->release ();
    }
    if (count >= 4) {
      break;
    }
    IOSleep (100);
  }
  IOSleep (500);
  int used = snprintf (gMediaReport, sizeof (gMediaReport),
                       "NATIVE_UFS media report: published=%u reads=%llu failures=%llu media=%u\n",
                       native_ufs_blocks_published, native_ufs_block_reads, native_ufs_block_failures, count);
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  while (iterator && used < int (sizeof (gMediaReport)) - 200) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) {
      break;
    }
    OSString *uuid = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSString *content = OSDynamicCast (OSString, media->getProperty ("Content"));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    used += snprintf (gMediaReport + used, sizeof (gMediaReport) - used, "%s %s size=%llx uuid=%s\n",
                      bsd ? bsd->getCStringNoCopy () : "-", content ? content->getCStringNoCopy () : "-",
                      size ? size->unsigned64BitValue () : 0ULL, uuid ? uuid->getCStringNoCopy () : "-");
  }
  if (iterator) {
    iterator->release ();
  }
  if (matching) {
    matching->release ();
  }
  panic ("%s", gMediaReport);
}

bool NativeUfs::start (IOService *provider)
{
  unsigned probe = 0, rootMode = 0;
  PE_parse_boot_argn ("native-ufs-probe", &probe, sizeof (probe));
  PE_parse_boot_argn ("native-ufs-root", &rootMode, sizeof (rootMode));
  native_ufs_probe_requested = probe != 0;
  // Default: match for observability only, never touch the controller. Hardware work runs once,
  // from the root-device personality; the ufshc nub match stays observation-only.
  if ((!probe && !rootMode) || !OSDynamicCast (IOPlatformExpertDevice, provider)) {
    return false;
  }
  unsigned diagnostic = 0;
  PE_parse_boot_argn ("native-ufs-diagnostics", &diagnostic, sizeof (diagnostic));
  gRootDiagnosticEnabled = diagnostic == 1;
  if (probe == 2) {
    inspect ();
  }
  if (probe == 8) {
    inspectContextBanks ();
  }
  if (probe == 7) {
    inspectSmmu ();
  }
  if (probe == 6) {
    dumpFirmwareList ();
  }
  if ((probe >= 3 && probe <= 5) || probe == 9) {
    adoptAndRead (probe);
  }
  if (probe == 10) {
    // Read-only check before anything is flashed: LUN 0 GPT -> "userdata" -> its first block.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root check: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, &why)) {
      panic ("NATIVE_UFS root check: %s", why);
    }
    panic ("NATIVE_UFS root check\n"
           "userdata first=%llx last=%llx\n"
           "block0 magic %08x %08x ext4magic=%04x ext4blocks=%u\n"
           "manifest status=%u windows=%u",
           layout->userdataFirst, layout->userdataLast, layout->magic0, layout->magic1,
           layout->ext4Magic, layout->ext4Blocks, layout->manifestStatus, layout->windows);
  }
  if (probe == 11) {
    // After the root image is flashed: manifest, then each window's own GPT (sector 1 of the
    // attached dmg disk image carries "EFI PART"). Read-only.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root windows: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, &why)) {
      panic ("NATIVE_UFS root windows: %s", why);
    }
    uint32_t sig[4][2] = {};
    uint32_t seen[4] = {};
    for (uint32_t index = 0; index < layout->windows && index < 4; index++) {
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[index].offset;
      if (hostRead4k (0, absolute / 4096, 1, gScratch) == kIOReturnSuccess) {
        seen[index] = 1;
        sig[index][0] = le32 (gScratch + 512);
        sig[index][1] = le32 (gScratch + 516);
      }
    }
    panic ("NATIVE_UFS root windows\n"
           "userdata first=%llx last=%llx manifest status=%u windows=%u\n"
           "w0 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w1 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w0 sha %08x %08x %08x %08x\n"
           "w1 sha %08x %08x %08x %08x",
           layout->userdataFirst, layout->userdataLast, layout->manifestStatus, layout->windows,
           layout->window[0].name, layout->window[0].offset, layout->window[0].size, sig[0][0], sig[0][1], seen[0],
           layout->window[1].name, layout->window[1].offset, layout->window[1].size, sig[1][0], sig[1][1], seen[1],
           be32w (layout->window[0].sha256), be32w (layout->window[0].sha256 + 4), be32w (layout->window[0].sha256 + 8), be32w (layout->window[0].sha256 + 12),
           be32w (layout->window[1].sha256), be32w (layout->window[1].sha256 + 4), be32w (layout->window[1].sha256 + 8), be32w (layout->window[1].sha256 + 12));
  }
  if (probe == 14) {
    // Immediate memory snapshot at root-device start, before the userspace that triggers jetsam.
    panic ("NATIVE_UFS early memory snapshot\n"
           "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
           "available=%u max_mem=%llx procs=%d",
           vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
           vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
           memorystatus_available_pages, max_mem, nprocs);
  }
  if (probe == 13) {
    unsigned delay = 12;
    PE_parse_boot_argn ("native-ufs-stats-delay", &delay, sizeof (delay));
    if (!startRoot (provider)) {
      panic ("NATIVE_UFS memory snapshot: root start failed");
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (statsThread, reinterpret_cast<void *> (static_cast<uintptr_t> (delay)), &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS memory snapshot: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 12) {
    bool published = startRoot (provider);
    if (!published) {
      panic ("NATIVE_UFS media report: root start failed (blocks=%u)", native_ufs_blocks_published);
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (mediaReportThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS media report: cannot start the report thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (rootMode && !probe) {
    return startRoot (provider);
  }
  return false;
}

// Boot the root image's windows as block devices. Soft-fails (logs, publishes nothing).
bool NativeUfs::startRoot (IOService *provider)
{
  const char *why = "";
  native_ufs_root_step = 1;
  if (!hostBringUp (&why)) {
    rootDiagnosticFailure (why);
    IOLog ("NativeUfs: root bring-up failed: %s\n", why);
    return false;
  }
  native_ufs_root_step = 2;
  RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
  if (!layout || !findRootLayout (layout, &why)) {
    rootDiagnosticFailure (layout ? why : "root layout allocation failed");
    IOLog ("NativeUfs: root layout failed: %s\n", why);
    if (layout) IOFree (layout, sizeof (RootLayout));
    return false;
  }
  native_ufs_root_step = 3;
  if (layout->manifestStatus != 0) {
    rootDiagnosticFailure ("root manifest rejected");
    IOLog ("NativeUfs: no valid root manifest in userdata (status %u, magic %08x %08x)\n",
           layout->manifestStatus, layout->magic0, layout->magic1);
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  gStoreGeometry = { true, layout->userdataFirst, layout->userdataLast, layout->imageBytes, layout->gptEntriesLba,
                     layout->gptHeaderCrc, layout->gptEntriesCrc, layout->gptEntryCount };
  gRootObjects = layout->objects;
  for (unsigned i = 0; i < layout->objects; ++i) gRootObject[i] = layout->object[i];
  gRootObjectBase = layout->userdataFirst * 4096ULL;
  const char *names[4] = {};
  for (unsigned i = 0; i < layout->windows; ++i) names[i] = layout->window[i].name;
  if (!ufsBuildRootPlan (names, layout->windows, gPublicationPlan)) {
    rootDiagnosticFailure ("root windows require distinct OS and SystemOS");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  if (!IOService::start (provider)) {
    rootDiagnosticFailure ("root IOService start failed");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  // Prepare all windows or publish none. Diagnostics never decide policy.
  gPending = 0;
  gPendingProvider = this;
  struct Operations {
    NativeUfs *owner;
    RootLayout *layout;
    bool prepare (unsigned index) {
      gRootDiagnostic.stage = "root window allocation/configuration";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Prepare);
      gRootDiagnostic.publicationWindow = index;
      gRootDiagnostic.windowBytes = layout->window[index].size;
      NativeUfsBlock *device = OSTypeAlloc (NativeUfsBlock);
      gPendingDevice[index] = device;
      gPendingAttached[index] = false;
      gPendingUuid[index][0] = 0;
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[index].offset;
      if (!device || !device->init (static_cast<OSDictionary *> (nullptr)) ||
          !device->configure (layout->window[index].name, absolute, layout->window[index].size)) return false;
      gRootDiagnostic.stage = "root window attachment";
      if (!device->attach (owner)) return false;
      gPendingAttached[index] = true;
      ++gPending;
      gRootDiagnostic.prepared = gPending;
      return true;
    }
    bool validate () {
      gRootDiagnostic.stage = "signed window identities";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Identity);
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      for (unsigned i = 0; i < gPending; ++i) {
        gRootDiagnostic.publicationWindow = i;
        if (!gPendingDevice[i]->signedVolumeUuid (gPendingUuid[i])) return false;
        for (unsigned j = 0; j < i; ++j) if (!strcmp (gPendingUuid[i], gPendingUuid[j])) return false;
      }
      return true;
#else
      return false; // Root publication requires owner verification in every build.
#endif
    }
    bool schedule () {
      gRootDiagnostic.stage = "root publisher thread";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Schedule);
      gRootDiagnostic.publicationWindow = 0xffffffff;
      thread_t thread = nullptr;
      owner->retain ();
      if (kernel_thread_start (publishThread, nullptr, &thread) != KERN_SUCCESS) {
        owner->release ();
        return false;
      }
      thread_deallocate (thread);
      return true;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations = { this, layout };
  UfsRootPublicationResult result = ufsPrepareRootWindows (operations, layout->windows);
  IOFree (layout, sizeof (RootLayout));
  if (result.phase != UfsRootPhase::Complete) {
    gPending = 0;
    gRootDiagnostic.result = result.phase == UfsRootPhase::Identity ? kIOReturnNotPrivileged :
      result.phase == UfsRootPhase::Schedule ? kIOReturnNoResources : kIOReturnError;
    IOLog ("NativeUfs: root preparation failed phase=%u window=%u; nothing published\n",
           static_cast<unsigned> (result.phase), result.index);
    IOService::stop (provider);
    rootDiagnosticFailure (result.phase == UfsRootPhase::Schedule ? "root publisher thread creation failed" :
                          result.phase == UfsRootPhase::Identity ? "signed window identities rejected" :
                          "root window preparation failed");
    return false;
  }
  return true;
}

void NativeUfs::stop (IOService *provider)
{
  fCmdDesc.reset ();
  fMap.reset ();
  IOService::stop (provider);
}
