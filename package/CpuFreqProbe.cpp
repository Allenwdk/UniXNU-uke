// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off CPU frequency control for the SM8550 EPSS frequency domains. `sheng-cpufreq=<n>`:
//   1  read-only: report each domain's enable bit, current/requested level and its highest LUT entry
//   2  additionally request that highest non-turbo level in domain 0 (the boot CPU's cluster)
//   3  the same in every enabled domain (including clusters whose cores are not yet started)
// Register layout is the qcom-cpufreq-hw "epss" data of the pinned mainline tree (reg_enable 0x0, reg_domain_state 0x20,
// reg_dcvs_ctrl 0xb0, reg_freq_lut 0x100 with 4-byte rows, reg_perf_state 0x320); the three domain windows come from the
// cpufreq@17d91000 node of sm8550.dtsi. LUT row: bits 31:30 source, 18:16 core count (1 = turbo), 7:0 L value; an L value
// is a multiple of the 19.2 MHz bi_tcxo_div2 clock, source 0 is GPLL0/2. Nothing here is validated on the device until a
// boot has shown it.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>

extern "C" {
char native_cpufreq_row[120];
volatile uint32_t native_cpufreq_stage;
}

namespace {
constexpr uint64_t kDomainBase[3] = {0x17d91000, 0x17d92000, 0x17d93000};
constexpr uint64_t kDomainSize = 0x1000;
constexpr uint32_t kRegEnable = 0x0, kRegDomainState = 0x20, kRegDcvsCtrl = 0xb0, kRegFreqLut = 0x100, kRegPerfState = 0x320;
constexpr unsigned kLutEntries = 40;
constexpr uint32_t kXoKHz = 19200;        // bi_tcxo_div2
constexpr uint32_t kGpllHalfKHz = 300000; // GPLL0 600 MHz / CLK_HW_DIV 2

uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t lutKHz(uint32_t row) {return (row >> 30) ? kXoKHz * (row & 0xff) : kGpllHalfKHz;}
}

extern "C" void sheng_cpufreq_probe(void) {
    unsigned mode = 0;
    PE_parse_boot_argn("sheng-cpufreq", &mode, sizeof(mode));
    if (!mode) return;
    native_cpufreq_stage = 1;
    char *out = native_cpufreq_row;
    unsigned n = (unsigned)snprintf(out, sizeof(native_cpufreq_row), "CPUFREQ");
    for (unsigned d = 0; d < 3; ++d) {
        IODeviceMemory *memory = IODeviceMemory::withRange(kDomainBase[d], kDomainSize);
        OSSharedPtr<IOMemoryMap> map(memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr, OSNoRetain);
        if (memory) memory->release();
        if (!map) {n += (unsigned)snprintf(out + n, sizeof(native_cpufreq_row) - n, " d%u:nomap", d); continue;}
        uintptr_t va = static_cast<uintptr_t>(map->getVirtualAddress());
        uint32_t enable = rd32(va, kRegEnable), state = rd32(va, kRegDomainState) & 0xff, perf = rd32(va, kRegPerfState);
        uint32_t dcvs = rd32(va, kRegDcvsCtrl);
        // Last distinct, non-turbo row before the table ends (two equal frequencies end it).
        int best = -1; uint32_t bestKHz = 0, prev = 0;
        for (unsigned i = 0; i < kLutEntries; ++i) {
            uint32_t row = rd32(va, kRegFreqLut + i * 4);
            uint32_t khz = lutKHz(row);
            bool turbo = ((row >> 16) & 7) == 1;
            if (i > 0 && khz == prev) break;
            if (!turbo && khz >= bestKHz) {best = (int)i; bestKHz = khz;}
            prev = khz;
        }
        int rc = 0;
        // Mode 2 writes only domain 0 (the boot CPU's own cluster); mode 3 also the clusters whose cores are still off.
        if (mode >= 2 && (d == 0 || mode >= 3) && (enable & 1) && best >= 0) {
            wr32(va, kRegPerfState, (uint32_t)best);
            if (dcvs & 1) for (unsigned c = 1; c < 4; ++c) wr32(va, kRegPerfState + c * 4, (uint32_t)best);
            rc = 1;
        }
        n += (unsigned)snprintf(out + n, sizeof(native_cpufreq_row) - n, " d%u:e%u c%u p%u m%d@%u%s",
                                d, enable & 1, state * kXoKHz / 1000, perf, best, bestKHz / 1000, rc ? " SET" : "");
    }
    native_cpufreq_stage = 2;
}
