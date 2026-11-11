// SPDX-License-Identifier: MIT
// SDAM reboot reason for the opt-in autoreset path in BootWatchdogPolicy.c.
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <kern/sched.h>
#include <pexpert/pexpert.h>
#include "BootResetCore.h"

// Uke stock vendor_boot.dts spmi0_bus and dtbo_0.dts restart_reason (SID 0, SDAM 0x7100, byte 0x48).
enum {SpmiCore = 0xc400000, SpmiCoreSize = 0x3000, SpmiObsrvr = 0xc440000, SpmiChnls = 0xc500000, SpmiCnfg = 0xc42d000, SpmiCnfgSize = 0x4000};
enum {PsciVersion = 0x84000000u, PsciFeatures = 0x8400000au, PsciSystemReset = 0x84000009u, PsciSystemReset2_32 = 0x84000012u,
      PsciSystemReset2_64 = 0xc4000012u};

static uint64_t psci(uint64_t fid, uint64_t a1) {
  register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = a1, x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                   "x14", "x15", "x16", "x17");
  return x0;
}

// ml_io_map hands out permanent mappings (io_submap, KMA_PERMANENT) that ml_io_unmap cannot free: its kmem_free on kernel_map panics with the
// map lock held (int-105 hung launchd that way). Mappings that are unmapped again must be unmappable ones (flags 7 = VM_WIMG_IO).
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
static volatile uint32_t *map(uint64_t phys, uint64_t size) {return (volatile uint32_t *)ml_io_map_unmappable((vm_offset_t)phys, (vm_size_t)size, 7);}

struct reset_maps {
  struct br_arbiter a;
  struct br_found f;
  volatile uint32_t *obs, *chn;
  uint64_t obs_size, chn_size;
  int find;   // br_find's result, -1 before it ran
};
static void unmap_all(struct reset_maps *m) {
  if (m->a.core) ml_io_unmap((vm_offset_t)m->a.core, SpmiCoreSize);
  if (m->a.cnfg) ml_io_unmap((vm_offset_t)m->a.cnfg, SpmiCnfgSize);
  if (m->obs) ml_io_unmap((vm_offset_t)m->obs, (vm_size_t)m->obs_size);
  if (m->chn) ml_io_unmap((vm_offset_t)m->chn, (vm_size_t)m->chn_size);
  memset(m, 0, sizeof *m);
}
// Maps the arbiter, finds the SDAM's APID and maps its observer (and, when asked, read/write) channels. 0 = ready.
static int open_maps(struct reset_maps *m, int want_write) {
  memset(m, 0, sizeof *m);
  m->find = -1;
  m->a.ee = 0;   // qcom,ee = <0>
  m->a.core = map(SpmiCore, SpmiCoreSize);
  m->a.cnfg = map(SpmiCnfg, SpmiCnfgSize);
  if (!m->a.core || !m->a.cnfg) return 1;
  m->find = br_find(&m->a, &m->f);
  if (m->find) return 2;
  m->obs_size = ((uint64_t)BR_OBS_APID_STRIDE * ((uint64_t)m->f.apid + 1) + 0xfff) & ~0xfffull;
  m->obs = map(SpmiObsrvr, m->obs_size);
  if (!m->obs) return 3;
  if (want_write) {
    m->chn_size = (uint64_t)BR_CHNL_APID_STRIDE * ((uint64_t)m->f.apid + 1);
    m->chn = map(SpmiChnls, m->chn_size);
    if (!m->chn) return 4;
  }
  return 0;
}

// The arbiter and the SDAM cell, read only. Fills text; returns 0 when the cell can be written (version known, APID found, owned by this EE).
int boot_reset_probe(char *text, unsigned cap) {
  const uint64_t ver = psci(PsciVersion, 0);
  const int64_t f_reset = (int64_t)psci(PsciFeatures, PsciSystemReset), f2_32 = (int64_t)psci(PsciFeatures, PsciSystemReset2_32),
                f2_64 = (int64_t)psci(PsciFeatures, PsciSystemReset2_64);
  struct reset_maps m;
  uint8_t cell = 0xff;
  int open_rc = open_maps(&m, 0), read_rc = -1;
  if (!open_rc) read_rc = br_read8(m.obs, &m.a, m.f.apid, BR_REBOOT_CELL, &cell);
  const int writable = !open_rc && !read_rc && m.f.owner == m.a.ee;
  snprintf(text, cap, "psci v%x reset=%lld reset2_32=%lld reset2_64=%lld | spmi v%08x apids=%u open=%d find=%d apid=%d owner=%d sdam48=%02x read=%d ok=%d",
           (unsigned)ver, (long long)f_reset, (long long)f2_32, (long long)f2_64, m.a.core ? m.f.version : 0, m.a.core ? m.f.apids : 0, open_rc,
           m.find, m.find ? -1 : m.f.apid, m.find ? -1 : m.f.owner, cell, read_rc, writable);
  unmap_all(&m);
  return writable ? 0 : 1;
}

// Writes "bootloader" into the reboot-reason cell and verifies it, without restarting. Done once early in an unattended run, it sends every
// later restart to fastboot, including a watchdog reset.
int boot_reset_mark_bootloader(char *text, unsigned cap) {
  struct reset_maps m;
  uint8_t before = 0xff, after = 0xff;
  int rc = open_maps(&m, 1);
  if (rc) {snprintf(text, cap, "mark refused: open=%d find=%d", rc, m.find); unmap_all(&m); return rc;}
  if (m.f.owner != m.a.ee) {snprintf(text, cap, "mark refused: apid %d is owned by EE %d", m.f.apid, m.f.owner); unmap_all(&m); return 5;}
  rc = br_set_bootloader(m.obs, m.chn, &m.a, m.f.apid, &before, &after);
  snprintf(text, cap, "mark: cell %02x -> %02x rc=%d", before, after, rc);
  unmap_all(&m);
  return rc ? 10 + rc : 0;
}
