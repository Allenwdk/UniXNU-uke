// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for the gated GPU power observation sequence (software register peer, not hardware).
#include "GpuPowerBoard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct model {
    uint32_t ahb, memnoc, dvm, vote, cx, gx, ccahb, ccsmmu, ccgmu, scr0, id0, id1, id2, gfsr, rbbm;
    uint32_t hwctrl;
    unsigned stuck_cx;   // 1: the CX domain never reports power-on
    unsigned cc_writes;
    uint32_t smr[8], s2cr[8];
    unsigned reads[5], writes, bad_window_reads;
    int deny_smmu;       // models the bus hanging: reading the SMMU when it must not be read marks a violation
    int violations;
    int fail_write;
};
static int rd(void *c, unsigned w, uint32_t o, uint32_t *out) {
    struct model *m = c;
    if (w > 4) return -1;
    m->reads[w]++;
    uint32_t v = 0;
    switch (w) {
    case GPB_WIN_GCC_GPU: v = o == 0x004 ? m->ahb : o == 0x010 ? m->memnoc : o == 0x018 ? m->dvm : 0xdead0001; break;
    case GPB_WIN_GCC_VOTE: v = m->vote; break;
    case GPB_WIN_GPUCC:
        if (!(m->ahb & GPB_CBCR_ENABLE)) m->violations++;       // GPUCC read with the AHB branch not enabled
        v = o == 0x953c ? m->hwctrl : o == 0x9108 ? m->cx : o == 0x905c ? m->gx : o == 0x911c ? m->ccahb : o == 0x7000 ? m->ccsmmu : o == 0x913c ? m->ccgmu : 0xdead0002; break;
    case GPB_WIN_SMMU:
        if (m->deny_smmu) m->violations++;
        v = o == 0 ? m->scr0 : o == 0x20 ? m->id0 : o == 0x24 ? m->id1 : o == 0x28 ? m->id2 : o == 0x48 ? m->gfsr :
            (o >= 0x800 && o < 0x820) ? m->smr[(o - 0x800) / 4] : (o >= 0xc00 && o < 0xc20) ? m->s2cr[(o - 0xc00) / 4] : 0xdead0003; break;
    case GPB_WIN_GPU:
        if (!(m->cx & GPB_GDSC_PWR_ON) || !(m->gx & GPB_GDSC_PWR_ON)) m->violations++;
        v = o == GPB_RBBM_STATUS_BYTES ? m->rbbm : 0xdead0004; break;
    }
    *out = v;
    return 0;
}
static int wr(void *c, unsigned w, uint32_t o, uint32_t v) {
    struct model *m = c;
    if (m->fail_write) return -1;
    if (w == GPB_WIN_GPUCC) {
        m->cc_writes++;
        if (o == 0x9108) {
            m->cx = v;
            if (!(v & 1) && !m->stuck_cx) {m->hwctrl = 0x80000000u; m->cx |= 0x80000000u;}
        } else if (o == 0x911c) m->ccahb = v;
        else if (o == 0x7000) m->ccsmmu = v & ~GPB_CBCR_CLK_OFF;
        else {m->violations++; return -1;}
        return 0;
    }
    if (w != GPB_WIN_GCC_GPU || o != GPB_GCC_GPU_CFG_AHB) {m->violations++; return -1;}
    m->writes++;
    m->ahb = v & ~GPB_CBCR_CLK_OFF;                  // the branch starts once enabled
    return 0;
}
#define CHECK(c) do { if (!(c)) {fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1);} } while (0)
#define ALL ((1u << 5) - 1)

static struct model base(void) {
    struct model m; memset(&m, 0, sizeof m);
    m.ahb = GPB_CBCR_CLK_OFF; m.memnoc = GPB_CBCR_CLK_OFF; m.dvm = GPB_CBCR_CLK_OFF; m.ccsmmu = GPB_CBCR_CLK_OFF;
    m.id0 = 0x00000004; m.id1 = 0x00000003; m.id2 = 0x0000002e; m.scr0 = 0x00fe0406;
    return m;
}

// Mode 6 peer: sparse GPUCC/GCC registers with the PLL lock, RCG root/update and branch CLK_OFF behaviour of the Linux drivers.
struct clk_model {
    uint32_t cc[0xa000 / 4], gcc[0x1000 / 4], vote;
    int no_lock, stuck_update, rcg_to_pll1_before_lock, writes;
    uint32_t pll0_writes;
};
static int crd(void *c, unsigned w, uint32_t o, uint32_t *out) {
    struct clk_model *m = c;
    *out = w == GPB_WIN_GPUCC ? m->cc[o / 4] : w == GPB_WIN_GCC_GPU ? m->gcc[o / 4] : w == GPB_WIN_GCC_VOTE ? m->vote : 0;
    return 0;
}
static int cwr(void *c, unsigned w, uint32_t o, uint32_t v) {
    struct clk_model *m = c;
    m->writes++;
    if (w == GPB_WIN_GCC_VOTE) {m->vote = v; return 0;}
    if (w == GPB_WIN_GCC_GPU) {m->gcc[o / 4] = v & ~GPB_CBCR_CLK_OFF; return 0;}
    if (o < 0x1000) m->pll0_writes++;
    if (o == GPB_CC_PLL1 + GPB_PLL_OPMODE && v == GPB_PLL_RUN && (m->cc[(GPB_CC_PLL1) / 4] & GPB_PLL_RESET_N) && !m->no_lock)
        m->cc[GPB_CC_PLL1 / 4] |= GPB_PLL_LOCK_DET;
    if (o == GPB_CC_GMU_RCGR + 4 && ((v >> 8) & 7) == 3 && !(m->cc[(GPB_CC_PLL1 + GPB_PLL_MODE) / 4] & GPB_PLL_OUTCTRL))
        m->rcg_to_pll1_before_lock = 1;
    if (o == GPB_CC_GMU_RCGR || o == GPB_CC_HUB_RCGR) {
        if (v & GPB_RCG_ROOT_EN) v &= ~GPB_RCG_ROOT_OFF;
        if ((v & GPB_RCG_UPDATE) && !m->stuck_update) v &= ~GPB_RCG_UPDATE;
    } else if (o == GPB_CC_PLL1 + GPB_PLL_MODE) v |= m->cc[o / 4] & GPB_PLL_LOCK_DET;
    else if (o >= 0x9000 && (v & GPB_CBCR_ENABLE)) v &= ~GPB_CBCR_CLK_OFF;
    m->cc[o / 4] = v;
    return 0;
}
static void clk_base(struct clk_model *m) {
    memset(m, 0, sizeof *m);
    m->cc[GPB_CC_PLL1 / 4] = 0x100;                 // int-011: MODE 0x100, OPMODE 0, L 0
    m->cc[GPB_CC_GMU_RCGR / 4] = 0; m->cc[GPB_CC_HUB_RCGR / 4] = 0;   // root on, XO
    m->cc[GPB_CC_CX_GMU_CLK / 4] = 1; m->cc[GPB_CC_CXO_CLK / 4] = 1; m->cc[GPB_CC_HUB_CX_INT_CLK / 4] = 1;
    m->cc[GPB_CC_DEMET_CLK / 4] = 1; m->cc[GPB_CC_AHB_CLK / 4] = 1; m->gcc[GPB_GCC_DDRSS_GPU_AXI / 4] = 1;
    m->gcc[GPB_GCC_GPU_MEMNOC_GFX / 4] = 0x80000001u;
}
static void clk_tests(void) {
    struct gpb_result r; memset(&r, 0, sizeof r); r.cc = GPB_DONE; r.cfg_ahb_after = 1; r.cx_gdscr = GPB_GDSC_PWR_ON;
    struct gpb_gmu_clocks k; struct gpb_gmu_clock_on o; struct clk_model m;
    // the int-011 state: full sequence, Linux values, PLL1 locked and outputs on before the GMU RCG selects it
    clk_base(&m); struct gpb_io io = {&m, crd, cwr, 0};
    gpb_read_gmu_clocks(&io, &r, &k);
    gpb_gmu_clocks_on(&io, &r, &k, &o);
    CHECK(o.step == 7 && !o.failed && !m.rcg_to_pll1_before_lock && o.lock_polls >= 1);
    CHECK((m.vote & GPB_GPLL0_VOTES) == GPB_GPLL0_VOTES);
    CHECK(m.cc[(GPB_CC_PLL1 + 0x10) / 4] == 0x44440016u && m.cc[(GPB_CC_PLL1 + 0x14) / 4] == 0xeaaa);
    CHECK(m.cc[(GPB_CC_PLL1 + 0x20) / 4] == 0x20485699u && m.cc[(GPB_CC_PLL1 + 0x38) / 4] == 0x34);
    CHECK(m.cc[(GPB_CC_PLL0 + 0x10) / 4] == 0x4444001eu && !(m.cc[GPB_CC_PLL0 / 4] & GPB_PLL_OUTCTRL) && m.cc[1] == 0);
    CHECK((m.cc[GPB_CC_PLL1 / 4] & (GPB_PLL_OUTCTRL | GPB_PLL_RESET_N)) == (GPB_PLL_OUTCTRL | GPB_PLL_RESET_N));
    CHECK((m.cc[(GPB_CC_PLL1 + GPB_PLL_USER_CTL) / 4] & 7) == 7 && m.cc[(GPB_CC_PLL1 + GPB_PLL_OPMODE) / 4] == 1);
    CHECK((o.hub_cfg & 0x7ff) == 0x505 && (o.gmu_cfg & 0x7ff) == 0x303);
    CHECK(!(m.cc[GPB_CC_GMU_RCGR / 4] & (GPB_RCG_ROOT_EN | GPB_RCG_UPDATE)) && (m.cc[GPB_CC_CXO_AON_CLK / 4] & 1));
    // no lock: stops at step 3, no RCG touched, GMU RCG still on XO
    clk_base(&m); m.no_lock = 1;
    gpb_read_gmu_clocks(&io, &r, &k); gpb_gmu_clocks_on(&io, &r, &k, &o);
    CHECK(o.step == 3 && o.failed == 1 && o.lock_polls == 1500 && m.cc[(GPB_CC_GMU_RCGR + 4) / 4] == 0);
    // an RCG that never takes the update stops at the hub step and leaves the GMU RCG alone
    clk_base(&m); m.stuck_update = 1;
    gpb_read_gmu_clocks(&io, &r, &k); gpb_gmu_clocks_on(&io, &r, &k, &o);
    CHECK(o.step == 4 && o.failed == 1 && o.hub_polls == 500 && m.cc[(GPB_CC_GMU_RCGR + 4) / 4] == 0);
    // a PLL1 already running is not reconfigured, and PLL0 is not touched
    clk_base(&m); m.cc[GPB_CC_PLL1 / 4] = GPB_PLL_OUTCTRL | GPB_PLL_RESET_N | GPB_PLL_LOCK_DET;
    m.cc[(GPB_CC_PLL1 + GPB_PLL_OPMODE) / 4] = 1; m.cc[(GPB_CC_PLL1 + 0x10) / 4] = 0x44440016u;
    gpb_read_gmu_clocks(&io, &r, &k); gpb_gmu_clocks_on(&io, &r, &k, &o);
    CHECK(o.step == 7 && !o.failed && !m.pll0_writes && !o.lock_polls);
    // CX off or no snapshot: nothing written
    clk_base(&m); r.cx_gdscr = 0;
    gpb_read_gmu_clocks(&io, &r, &k); gpb_gmu_clocks_on(&io, &r, &k, &o);
    CHECK(o.failed && !m.writes);
    printf("PASS: 5 GMU clock bring-up cases (Linux PLL/RCG/branch values and order, lock and update timeouts, running PLL, CX off)\n");
}

int main(void) {
    clk_tests();
    struct gpb_result r;
    // mode 0: nothing runs and nothing is read
    { struct model m = base(); struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 0, ALL, &r);
      CHECK(r.gcc == GPB_SKIPPED_MODE && !m.reads[0] && !m.writes); }
    // mode 1 with the AHB clock off: GCC read, no write, GPUCC/SMMU/GPU skipped, no violation
    { struct model m = base(); struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 1, ALL, &r);
      CHECK(r.gcc == GPB_DONE && r.cc == GPB_SKIPPED_PREREQ && r.smmu == GPB_SKIPPED_PREREQ && r.gpu == GPB_SKIPPED_PREREQ);
      CHECK(m.writes == 0 && !m.reads[GPB_WIN_GPUCC] && !m.violations && r.cfg_ahb == GPB_CBCR_CLK_OFF); }
    // mode 2 with the AHB clock off: the single Linux-style enable write, then GPUCC is read; rails off keep SMMU/GPU skipped
    { struct model m = base(); struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 2, ALL, &r);
      CHECK(m.writes == 1 && r.enabled_ahb && gpb_cfg_ahb_enabled(r.cfg_ahb_after) && r.cc == GPB_DONE);
      CHECK(r.smmu == GPB_SKIPPED_PREREQ && r.gpu == GPB_SKIPPED_PREREQ && !m.reads[GPB_WIN_SMMU] && !m.violations); }
    // a refused write is reported and GPUCC stays unread
    { struct model m = base(); m.fail_write = 1; struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 2, ALL, &r);
      CHECK(r.io_error && r.cc == GPB_SKIPPED_PREREQ && !m.reads[GPB_WIN_GPUCC]); }
    // AHB already on, CX on, SMMU vote clock on: SMMU gr0 and the valid-SMR scan run; GX off keeps the GPU window unread
    { struct model m = base(); m.ahb = 1; m.cx = GPB_GDSC_PWR_ON; m.ccsmmu = 1;
      m.smr[1] = 0x80000000u | 0x10; m.s2cr[1] = 0x00020003; m.smr[3] = 0x80000000u | 0x20;
      struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 1, ALL, &r);
      CHECK(r.smmu == GPB_DONE && r.id0 == 4 && r.id1 == 3 && r.scr0 == 0x00fe0406);
      CHECK(r.smr_valid == 2 && r.smr_first == 1 && r.s2cr_first_val == 0x00020003 && m.writes == 0);
      CHECK(r.gpu == GPB_SKIPPED_PREREQ && !m.reads[GPB_WIN_GPU] && !m.violations); }
    // the int-003 hardware value: enabled with hardware clock gating (0x80000003) reads CLK_OFF=1 but must proceed, with no write
    { struct model m = base(); m.ahb = 0x80000003u; m.cx = GPB_GDSC_PWR_ON; struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 2, ALL, &r);
      CHECK(m.writes == 0 && !r.enabled_ahb && r.cc == GPB_DONE && r.smmu == GPB_SKIPPED_PREREQ && !m.violations); }
    // CX on but the SMMU vote clock off: the SMMU must not be read
    { struct model m = base(); m.ahb = 1; m.cx = GPB_GDSC_PWR_ON; m.deny_smmu = 1; struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 1, ALL, &r);
      CHECK(r.smmu == GPB_SKIPPED_PREREQ && !m.reads[GPB_WIN_SMMU] && !m.violations); }
    // everything on: RBBM_STATUS read
    { struct model m = base(); m.ahb = 1; m.cx = m.gx = GPB_GDSC_PWR_ON; m.ccsmmu = 1; m.rbbm = 0x800000; struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 1, ALL, &r);
      CHECK(r.gpu == GPB_DONE && r.rbbm_status == 0x800000 && !m.violations); }
    // missing windows are reported, never read
    { struct model m = base(); m.ahb = 1; m.cx = GPB_GDSC_PWR_ON; m.ccsmmu = 1; struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 1, (1u << GPB_WIN_GCC_GPU) | (1u << GPB_WIN_GPUCC), &r);
      CHECK(r.smmu == GPB_NOMAP && r.gpu == GPB_NOMAP && !m.reads[GPB_WIN_SMMU]);
      gpb_run(&io, 1, 0, &r);
      CHECK(r.gcc == GPB_NOMAP); }
    // SMR scan is capped and an implausible NUMSMRG cannot run past the page
    { struct model m = base(); m.ahb = 1; m.cx = GPB_GDSC_PWR_ON; m.ccsmmu = 1; m.id0 = 0xff;
      struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 1, ALL, &r);
      CHECK(m.reads[GPB_WIN_SMMU] <= 5 + GPB_SMMU_MAX_SMR * 2); }
    // mode 3: CX off -> the gdsc sequence runs, the SMMU clock is enabled, then the SMMU global space is read (never written)
    { struct model m = base(); m.ahb = 0x80000003u; m.cx = 0x0022f801u; m.gx = 0x0022f801u; m.ccsmmu = GPB_CBCR_CLK_OFF;
      struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 3, ALL, &r);
      CHECK(r.powered_cx && r.powered_clocks && gpb_gdsc_on(r.cx_gdscr) && r.cx_before == 0x0022f801u && m.cc_writes >= 5);
      CHECK(!(m.cx & 1) && (m.cx & GPB_GDSC_RETAIN_MEM) && (m.cx & GPB_GDSC_RETAIN_FF));
      CHECK(r.smmu == GPB_DONE && r.gpu == GPB_SKIPPED_PREREQ && !m.violations && m.writes == 0); }
    // mode 2 must never write GPUCC even with CX off
    { struct model m = base(); m.ahb = 0x80000003u; m.cx = 0x0022f801u; struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 2, ALL, &r);
      CHECK(!m.cc_writes && !r.powered_cx && r.smmu == GPB_SKIPPED_PREREQ); }
    // mode 3 with a CX domain that never reports power-on: no clock write, SMMU not read, bounded polling
    { struct model m = base(); m.ahb = 0x80000003u; m.cx = 0x0022f801u; m.stuck_cx = 1; struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 3, ALL, &r);
      CHECK(!r.powered_cx && !r.powered_clocks && r.smmu == GPB_SKIPPED_PREREQ && m.cc_writes == 1 && !m.reads[GPB_WIN_SMMU]); }
    // mode 3 with CX already on makes no write
    { struct model m = base(); m.ahb = 0x80000003u; m.cx = GPB_GDSC_PWR_ON; m.ccsmmu = 1; struct gpb_io io = {&m, rd, wr, 0};
      gpb_run(&io, 3, ALL, &r);
      CHECK(!m.cc_writes && r.smmu == GPB_DONE); }
    // mode 5 clock snapshot: reads only, never with GPUCC unreadable; the GMU blocks stay unread while the GMU clock is off
    { struct model m = base(); struct gpb_io io = {&m, rd, wr, 0}; struct gpb_gmu_clocks k;
      gpb_run(&io, 1, ALL, &r);
      unsigned before = m.reads[GPB_WIN_GPUCC];
      gpb_read_gmu_clocks(&io, &r, &k);
      CHECK(!k.done && m.reads[GPB_WIN_GPUCC] == before && !gpb_gmu_regs_readable(&r, &k)); }
    { struct model m = base(); m.ahb = 0x80000003u; m.cx = 0x0022f801u; m.ccgmu = GPB_CBCR_CLK_OFF; m.vote = 1u << 15;
      struct gpb_io io = {&m, rd, wr, 0}; struct gpb_gmu_clocks k;
      gpb_run(&io, 3, ALL, &r);
      unsigned writes = m.cc_writes + m.writes, before = m.reads[GPB_WIN_GPUCC];
      gpb_read_gmu_clocks(&io, &r, &k);
      CHECK(k.done && m.cc_writes + m.writes == writes && m.reads[GPB_WIN_GPUCC] == before + 12 && !m.violations);
      CHECK(k.cx_gmu == GPB_CBCR_CLK_OFF && gpb_branch_digit(k.cx_gmu) == 2 && k.axi == 0xdead0001u && k.gpll0_vote == 1u << 15);
      CHECK(!gpb_gmu_regs_readable(&r, &k));
      k.cx_gmu = 1; r.cc_ahb = 1;
      CHECK(gpb_gmu_regs_readable(&r, &k) && gpb_branch_digit(1) == 1 && gpb_branch_digit(0x80000001u) == 3); }
    printf("PASS: 16 gated-sequence cases over a software register peer (mode gating, single Linux-style AHB enable, prerequisite skips, window absence, SMR scan cap)\n");
    return 0;
}
