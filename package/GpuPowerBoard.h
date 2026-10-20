// SPDX-License-Identifier: BSD-2-Clause-Patent
// sheng (SM8550) Adreno power/clock observation: board facts plus the staged, gated read sequence. Every offset names its
// source. Nothing here touches hardware; GpuPowerProbe.cpp owns the MMIO. Host-tested by GpuPowerBoardTests.c.
//
// Why staged: the GPU SMMU, GPUCC and the GPU register window sit behind clocks and power domains that Darwin has no driver
// for. Reading a block whose clock or rail is off can hang the bus (reset) instead of failing, so each stage runs only if the
// previous one showed its prerequisite on, and the first hardware boot learns the whole chain in one photo.
#ifndef SHENG_GPU_POWER_BOARD_H
#define SHENG_GPU_POWER_BOARD_H
#include <stdint.h>

// sm8550.dtsi (pinned ianchb/sm8550-mainline 42f3b40c): clock-controller@100000 (GCC), gpucc: clock-controller@3d90000 size 0xa000,
// adreno_smmu: iommu@3da0000 size 0x40000, gpu: gpu@3d00000 size 0x40000.
#define GPB_GCC_BASE 0x00100000ULL
#define GPB_GCC_SIZE 0x1000ULL            // only the 0x71000 page and 0x52000 vote page are read, mapped separately
#define GPB_GCC_GPU_PAGE 0x00171000ULL    // GCC_BASE + 0x71000
#define GPB_GCC_VOTE_PAGE 0x00152000ULL   // GCC_BASE + 0x52000
#define GPB_GPUCC_BASE 0x03d90000ULL
#define GPB_GPUCC_SIZE 0xa000ULL
#define GPB_SMMU_BASE 0x03da0000ULL
#define GPB_SMMU_GR0_SIZE 0x1000ULL       // global space 0 only: the apps_smmu abort happened on the first gr1 read
#define GPB_GPU_BASE 0x03d00000ULL
#define GPB_GPU_WINDOW 0x1000ULL          // one page is enough for RBBM_STATUS

// gcc-sm8550.c: gcc_gpu_cfg_ahb_clk 0x71004 is enabled by the driver probe (qcom_branch_set_clk_en(regmap, 0x71004));
// gcc_gpu_memnoc_gfx_clk halt/enable 0x71010 (voted); gcc_gpu_snoc_dvm_gfx_clk 0x71018; gcc_gpu_gpll0_clk_src enable 0x52000 bit 15,
// gcc_gpu_gpll0_div_clk_src bit 16.
// Offsets below are relative to the mapped page (0x71000 for the GPU branches, 0x52000 for the PLL vote register).
#define GPB_GCC_GPU_CFG_AHB 0x004
#define GPB_GCC_GPU_MEMNOC_GFX 0x010
#define GPB_GCC_GPU_SNOC_DVM_GFX 0x018
#define GPB_GCC_PLL_VOTE 0x000
#define GPB_CBCR_CLK_OFF (1u << 31)       // clk-branch.c: BRANCH_CLK_OFF; a running branch reads 0
#define GPB_CBCR_ENABLE (1u << 0)

// gpucc-sm8550.c: gpu_cc_cx_gdsc gdscr 0x9108, gpu_cc_gx_gdsc gdscr 0x905c; gpu_cc_ahb_clk 0x911c; gpu_cc_cx_gmu_clk 0x913c;
// gpu_cc_hlos1_vote_gpu_smmu_clk 0x7000. gdsc.c: PWR_ON_MASK BIT(31), SW_COLLAPSE_MASK BIT(0).
#define GPB_CC_CX_GDSCR 0x9108
#define GPB_CC_GX_GDSCR 0x905c
#define GPB_CC_AHB_CLK 0x911c
#define GPB_CC_CX_GMU_CLK 0x913c
#define GPB_CC_SMMU_VOTE_CLK 0x7000
#define GPB_CC_CX_GDS_HW_CTRL 0x953c      // gpu_cc_cx_gdsc.gds_hw_ctrl: with it set, gdsc.c polls PWR_ON here (VOTABLE)
#define GPB_GDSC_RETAIN_MEM (1u << 14)    // gdsc.c RETAIN_MEM / RETAIN_PERIPH, set by gdsc_force_mem_on; RETAIN_FF_ENABLE bit 11
#define GPB_GDSC_RETAIN_PERIPH (1u << 13)
#define GPB_GDSC_RETAIN_FF (1u << 11)
#define GPB_GDSC_POLL_US 2000             // STATUS_POLL_TIMEOUT_US in gdsc.c
#define GPB_GDSC_PWR_ON (1u << 31)
#define GPB_GDSC_SW_COLLAPSE (1u << 0)

// ARM SMMUv2 global space 0 (IHI 0062D.c): sCR0 0x0, ID0 0x20, ID1 0x24, ID2 0x28, sGFSR 0x48. Matches the reads of the UFS probe.
#define GPB_SMMU_SCR0 0x0
#define GPB_SMMU_ID0 0x20
#define GPB_SMMU_ID1 0x24
#define GPB_SMMU_ID2 0x28
#define GPB_SMMU_GFSR 0x48
#define GPB_SMMU_SMR(n) (0x800 + 4 * (n))
#define GPB_SMMU_S2CR(n) (0xc00 + 4 * (n))
#define GPB_SMMU_MAX_SMR 128             // GR0 is 0x1000 bytes: 0x800..0xbff holds 256 SMRs, the scan is capped

// a6xx.xml: RBBM_STATUS dword offset 0x0210 (A6XX-A7XX); the Adreno register accessors in this tree scale dword offsets by 4.
#define GPB_RBBM_STATUS_BYTES (0x0210 * 4)

// Stages. A stage records "skipped" with a reason instead of reading when its prerequisite is not met.
enum gpb_state {GPB_NOT_RUN = 0, GPB_DONE = 1, GPB_SKIPPED_PREREQ = 2, GPB_SKIPPED_MODE = 3, GPB_NOMAP = 4};

struct gpb_io {
    void *cookie;
    int (*read32)(void *cookie, unsigned window, uint32_t offset, uint32_t *out);   // 0 on success
    int (*write32)(void *cookie, unsigned window, uint32_t offset, uint32_t value);
    void (*delay_us)(void *cookie, unsigned us);                                     // may be NULL (host tests that need none)
};
enum {GPB_WIN_GCC_GPU = 0, GPB_WIN_GCC_VOTE = 1, GPB_WIN_GPUCC = 2, GPB_WIN_SMMU = 3, GPB_WIN_GPU = 4};

struct gpb_result {
    unsigned mode;                       // 1 read-only; 2 additionally enable gcc_gpu_cfg_ahb_clk if it is off (as Linux does)
    enum gpb_state gcc, cc, smmu, gpu;
    uint32_t cfg_ahb, cfg_ahb_after, memnoc, dvm, pll_vote;
    uint32_t cx_gdscr, gx_gdscr, cc_ahb, cc_smmu, cc_gmu;
    uint32_t scr0, id0, id1, id2, gfsr;
    uint32_t smr_valid, smr_first, smr_first_val, s2cr_first_val;
    uint32_t rbbm_status;
    uint32_t cx_before, cx_hwctrl, cc_smmu_before;   // mode 3: GDSCR before the vote, the polled hw_ctrl word, smmu clock before
    unsigned powered_cx, powered_clocks;             // mode 3 actions taken (1 = the sequence ran and the status confirmed)
    unsigned enabled_ahb;                // 1 if mode 2 wrote the enable bit
    int io_error;
};

static inline int gpb_clock_on(uint32_t cbcr) {return (cbcr & GPB_CBCR_CLK_OFF) == 0;}
// gcc_gpu_cfg_ahb_clk has hwcg_bit 1 (bit 1): with hardware clock gating an idle branch reads CLK_OFF=1 even though it is enabled
// and wakes on access (docs/native-ufs.md recorded the same for the UFS branches). int-003 read 0x80000003 here, so CLK_OFF is not
// a usable "off" test for it; the enable bit (bit 0) is. Voted branches without hwcg (gpu_cc_hlos1_vote_gpu_smmu_clk) keep CLK_OFF.
static inline int gpb_cfg_ahb_enabled(uint32_t cbcr) {return (cbcr & GPB_CBCR_ENABLE) != 0;}
static inline int gpb_gdsc_on(uint32_t gdscr) {return (gdscr & GPB_GDSC_PWR_ON) != 0;}
static inline unsigned gpb_smmu_num_smr(uint32_t id0) {return id0 & 0xff;}        // IDR0.NUMSMRG
static inline unsigned gpb_smmu_num_cb(uint32_t id1) {return id1 & 0xff;}         // IDR1.NUMCB

static inline int gpb_rd(const struct gpb_io *io, struct gpb_result *r, unsigned window, uint32_t offset, uint32_t *out) {
    uint32_t v = 0;
    if (io->read32(io->cookie, window, offset, &v)) {r->io_error = 1; *out = 0; return -1;}
    *out = v;
    return 0;
}

// Runs the gated sequence. `have` is a bitmask of mapped windows (1 << GPB_WIN_*); a missing window makes its stage GPB_NOMAP.
static inline void gpb_run(const struct gpb_io *io, unsigned mode, unsigned have, struct gpb_result *r) {
    __builtin_memset(r, 0, sizeof *r);
    r->mode = mode;
    if (!mode || !(have & (1u << GPB_WIN_GCC_GPU))) {r->gcc = (mode ? GPB_NOMAP : GPB_SKIPPED_MODE); return;}
    // Stage 1: GCC. Always safe: the same window family the touch and UFS probes already read.
    if (gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_CFG_AHB, &r->cfg_ahb)) return;
    gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MEMNOC_GFX, &r->memnoc);
    gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_SNOC_DVM_GFX, &r->dvm);
    if (have & (1u << GPB_WIN_GCC_VOTE)) gpb_rd(io, r, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &r->pll_vote);
    r->cfg_ahb_after = r->cfg_ahb;
    if (!gpb_cfg_ahb_enabled(r->cfg_ahb) && mode >= 2) {
        // The single write Linux's gcc-sm8550 probe makes (qcom_branch_set_clk_en); mode 1 never writes.
        if (!io->write32(io->cookie, GPB_WIN_GCC_GPU, GPB_GCC_GPU_CFG_AHB, r->cfg_ahb | GPB_CBCR_ENABLE)) {
            r->enabled_ahb = 1;
            gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_CFG_AHB, &r->cfg_ahb_after);
        } else r->io_error = 1;
    }
    r->gcc = GPB_DONE;
    // Stage 2: GPUCC needs gcc_gpu_cfg_ahb_clk running.
    if (!(have & (1u << GPB_WIN_GPUCC))) r->cc = GPB_NOMAP;
    else if (!gpb_cfg_ahb_enabled(r->cfg_ahb_after)) r->cc = GPB_SKIPPED_PREREQ;
    else {
        gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, &r->cx_gdscr);
        gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &r->gx_gdscr);
        gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &r->cc_ahb);
        gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &r->cc_smmu);
        gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, &r->cc_gmu);
        r->cc = GPB_DONE;
    }
    // Mode 3 (writes, mirrors gdsc.c gdsc_enable for gpu_cc_cx_gdsc and the three clock enables Linux's adreno-smmu needs): only when
    // GPUCC was read, CX is off and the stage-2 snapshot is sane. GX is never touched here (the GMU owns it).
    r->cx_before = r->cx_gdscr; r->cc_smmu_before = r->cc_smmu;
    if (mode >= 3 && r->cc == GPB_DONE && !gpb_gdsc_on(r->cx_gdscr)) {
        uint32_t v = r->cx_gdscr, st = 0;
        int bad = io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, v & ~GPB_GDSC_SW_COLLAPSE);   // gdsc_update_collapse_bit(off)
        if (!bad) {
            unsigned waited = 0;
            if (io->delay_us) io->delay_us(io->cookie, 1);
            for (;;) {                                                      // gdsc_poll_status(GDSC_ON) on gds_hw_ctrl bit 31
                if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GDS_HW_CTRL, &st)) {bad = 1; break;}
                if (gpb_gdsc_on(st) || waited >= GPB_GDSC_POLL_US) break;
                if (io->delay_us) io->delay_us(io->cookie, 10);
                waited += 10;
            }
            r->cx_hwctrl = st;
            if (!bad && gpb_gdsc_on(st)) {
                gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, &v);          // gdsc_force_mem_on, then retain_ff_on, 1 us apart
                bad |= io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, v | GPB_GDSC_RETAIN_MEM | GPB_GDSC_RETAIN_PERIPH);
                if (io->delay_us) io->delay_us(io->cookie, 1);
                bad |= io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, v | GPB_GDSC_RETAIN_MEM | GPB_GDSC_RETAIN_PERIPH | GPB_GDSC_RETAIN_FF);
                if (io->delay_us) io->delay_us(io->cookie, 1);
                gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, &r->cx_gdscr);
                r->powered_cx = !bad && gpb_gdsc_on(r->cx_hwctrl);
            }
        }
        if (bad) r->io_error = 1;
        if (r->powered_cx) {
            // gpu_cc_ahb_clk (enable 0x911c bit 0, halt-delay) and gpu_cc_hlos1_vote_gpu_smmu_clk (0x7000 bit 0, voted), as the
            // adreno_smmu DT node lists them; then wait for the voted branch's CLK_OFF to clear.
            uint32_t a = 0, c = 0;
            gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &a);
            gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &c);
            int cbad = io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, a | GPB_CBCR_ENABLE);
            cbad |= io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, c | GPB_CBCR_ENABLE);
            unsigned waited = 0;
            for (;;) {
                if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &r->cc_smmu)) {cbad = 1; break;}
                if (gpb_clock_on(r->cc_smmu) || waited >= 500) break;
                if (io->delay_us) io->delay_us(io->cookie, 10);
                waited += 10;
            }
            gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &r->cc_ahb);
            r->powered_clocks = !cbad && gpb_clock_on(r->cc_smmu);
            if (cbad) r->io_error = 1;
        }
    }
    // Stage 3: the GPU SMMU needs the CX power domain and its HLOS vote clock (adreno_smmu DT: power-domains = CX_GDSC, clocks
    // hlos = GPU_CC_HLOS1_VOTE_GPU_SMMU_CLK). The bus/iface clocks are not required to be on for a global-space read; this is a
    // conservative guess made visible in the rows, not a validated rule. Global space 0 only.
    if (!(have & (1u << GPB_WIN_SMMU))) r->smmu = GPB_NOMAP;
    else if (r->cc != GPB_DONE || !gpb_gdsc_on(r->cx_gdscr) || !gpb_clock_on(r->cc_smmu)) r->smmu = GPB_SKIPPED_PREREQ;
    else {
        gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_SCR0, &r->scr0);
        gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_ID0, &r->id0);
        gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_ID1, &r->id1);
        gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_ID2, &r->id2);
        gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_GFSR, &r->gfsr);
        unsigned n = gpb_smmu_num_smr(r->id0);
        if (n > GPB_SMMU_MAX_SMR) n = GPB_SMMU_MAX_SMR;
        r->smr_first = 0xffffffffu;
        for (unsigned i = 0; i < n; ++i) {
            uint32_t smr = 0;
            if (gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_SMR(i), &smr)) break;
            if (smr & (1u << 31)) {                      // SMR.VALID
                ++r->smr_valid;
                if (r->smr_first == 0xffffffffu) {
                    r->smr_first = i; r->smr_first_val = smr;
                    gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_S2CR(i), &r->s2cr_first_val);
                }
            }
        }
        r->smmu = GPB_DONE;
    }
    // Stage 4: the GPU register window needs the GX domain on (RBBM_STATUS is in the GX-gated space on A7XX) and CX on.
    if (!(have & (1u << GPB_WIN_GPU))) r->gpu = GPB_NOMAP;
    else if (r->cc != GPB_DONE || !gpb_gdsc_on(r->cx_gdscr) || !gpb_gdsc_on(r->gx_gdscr)) r->gpu = GPB_SKIPPED_PREREQ;
    else {
        gpb_rd(io, r, GPB_WIN_GPU, GPB_RBBM_STATUS_BYTES, &r->rbbm_status);
        r->gpu = GPB_DONE;
    }
}

// Mode 5: read-only snapshot of the clocks a6xx_gmu_resume needs before a GMU cold start (DT gmu clocks ahb, gmu, cxo, axi,
// memnoc, hub, demet) and of their sources. Linux sets gmu_clk_src to 200 MHz (the table rounds to 220 MHz = GPU_CC_PLL1 / 2,
// parent_map_1 sel 3) and hub_clk_src to 200 MHz (GPLL0 / 3, sel 5); gpucc probe configures PLL1 (L 0x16, alpha 0xeaaa) and
// enables CXO_AON 0x9004 / DEMET 0x900c. Offsets: gpucc-sm8550.c and clk-alpha-pll.c (LUCID_OLE) at the pinned 42f3b40c.
#define GPB_CC_PLL1 0x1000                // gpu_cc_pll1.offset; MODE +0, OPMODE +4, L_VAL +0x10, USER_CTL +0x18
#define GPB_PLL_MODE 0x0
#define GPB_PLL_OPMODE 0x4
#define GPB_PLL_L_VAL 0x10
#define GPB_PLL_USER_CTL 0x18
#define GPB_PLL_LOCK_DET (1u << 31)       // clk-alpha-pll.c PLL_LOCK_DET
#define GPB_CC_GMU_RCGR 0x9318            // gpu_cc_gmu_clk_src cmd_rcgr; CFG at +4 (clk-rcg2.c: SRC_DIV 4:0, SRC_SEL 10:8)
#define GPB_CC_HUB_RCGR 0x93ec            // gpu_cc_hub_clk_src
#define GPB_RCG_ROOT_OFF (1u << 31)       // clk-rcg2.c CMD_ROOT_OFF
#define GPB_CC_CXO_CLK 0x9144
#define GPB_CC_HUB_CX_INT_CLK 0x9148
#define GPB_CC_DEMET_CLK 0x900c
#define GPB_GCC_DDRSS_GPU_AXI 0x154       // gcc-sm8550.c gcc_ddrss_gpu_axi_clk 0x71154, in the 0x71000 page

struct gpb_gmu_clocks {
    uint32_t pll1_mode, pll1_opmode, pll1_l, pll1_user;
    uint32_t gmu_cmd, gmu_cfg, hub_cmd, hub_cfg;
    uint32_t cx_gmu, cxo, hub_cx_int, demet, axi, memnoc, gpll0_vote;
    int done;                            // 0: GPUCC was not readable, nothing was read
};
// A branch digit for the banner: bit 0 enable, bit 1 CLK_OFF.
static inline unsigned gpb_branch_digit(uint32_t cbcr) {return (cbcr & GPB_CBCR_ENABLE) | ((cbcr >> 30) & 2);}
// The GMU register blocks (gmu core, RSCC, CX_MISC) are read only when CX is on and both the GPUCC AHB clock and
// gpu_cc_cx_gmu_clk already run; nothing in mode 5 turns them on.
static inline int gpb_gmu_regs_readable(const struct gpb_result *r, const struct gpb_gmu_clocks *k) {
    return k->done && gpb_gdsc_on(r->cx_gdscr) && gpb_clock_on(r->cc_ahb) && gpb_clock_on(k->cx_gmu);
}
static inline void gpb_read_gmu_clocks(const struct gpb_io *io, struct gpb_result *r, struct gpb_gmu_clocks *k) {
    __builtin_memset(k, 0, sizeof *k);
    if (r->cc != GPB_DONE || !gpb_cfg_ahb_enabled(r->cfg_ahb_after)) return;
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_MODE, &k->pll1_mode);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_OPMODE, &k->pll1_opmode);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_L_VAL, &k->pll1_l);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_USER_CTL, &k->pll1_user);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_GMU_RCGR, &k->gmu_cmd);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_GMU_RCGR + 4, &k->gmu_cfg);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_HUB_RCGR, &k->hub_cmd);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_HUB_RCGR + 4, &k->hub_cfg);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, &k->cx_gmu);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CXO_CLK, &k->cxo);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_HUB_CX_INT_CLK, &k->hub_cx_int);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_DEMET_CLK, &k->demet);
    gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_DDRSS_GPU_AXI, &k->axi);
    k->memnoc = r->memnoc;
    // Read again (int-012 showed the stale stage-1 word after the bring-up had set the votes).
    if (gpb_rd(io, r, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &k->gpll0_vote)) k->gpll0_vote = r->pll_vote;
    k->done = 1;
}

// Mode 6: the GMU clock bring-up a6xx_gmu_resume relies on, in Linux order (int-011 read PLL1 MODE 0x100 OPMODE 0 L 0,
// both RCGs on XO, the GMU branches already enabled and the GPLL0 GPU votes clear):
//   1 gcc_gpu_gpll0_clk_src / _div_clk_src votes (0x52000 bits 15/16, BRANCH_HALT_DELAY 10 us)
//   2 clk_lucid_ole_pll_configure for gpu_cc_pll0 and gpu_cc_pll1 (the gpucc probe; PLL0 is only configured, as there)
//   3 alpha_pll_lucid_evo_enable for PLL1 (RESET_N, OPMODE RUN, LOCK_DET within 1500 us, USER_CTL outputs, OUTCTRL)
//   4 gpu_cc_hub_clk_src 200 MHz (GPLL0 sel 5, div 3 -> 5) and 5 gpu_cc_gmu_clk_src 220 MHz (PLL1 sel 3, div 2 -> 3), each by
//     clk_rcg2_shared_force_enable_clear: ROOT_EN, wait ROOT_OFF clear, CFG, CMD_UPDATE until it clears (500 us), ROOT_EN off
//   6 qcom_branch_set_clk_en for CXO_AON 0x9004 / DEMET 0x900c, then the seven DT GMU branches, waiting for cx_gmu CLK_OFF
// A PLL that already runs is not reconfigured; any timeout stops the sequence at its step.
#define GPB_CC_PLL0 0x0000
#define GPB_CC_CXO_AON_CLK 0x9004
#define GPB_PLL_OUTCTRL (1u << 0)
#define GPB_PLL_RESET_N (1u << 2)
#define GPB_PLL_OUT_MASK 0x7u
#define GPB_PLL_RUN 1u
#define GPB_RCG_UPDATE (1u << 0)
#define GPB_RCG_ROOT_EN (1u << 1)
#define GPB_RCG_CFG_CLEAR (0x1fu | (7u << 8) | (3u << 12) | (1u << 20))   // hid 5 bits, SRC_SEL, MODE, HW_CLK_CTRL
#define GPB_GPLL0_VOTES ((1u << 15) | (1u << 16))

struct gpb_gmu_clock_on {
    unsigned step;                       // last step started (1..6), 7 when all passed
    int failed;                          // 1 timeout or refusal at `step`, 2 I/O error
    unsigned lock_polls, hub_polls, gmu_polls;
    uint32_t pll1_mode, pll1_opmode, hub_cfg, gmu_cfg, cx_gmu;
};
// lucid_ole configs from gpucc-sm8550.c: offsets L 0x10 .. TEST_CTL_U2 0x38; clk_alpha_pll_write_config skips zero values.
static inline int gpb_pll_configure(const struct gpb_io *io, struct gpb_result *r, uint32_t base, uint32_t l, uint32_t alpha) {
    static const uint32_t off[] = {0x10, 0x14, 0x20, 0x24, 0x28, 0x18, 0x1c, 0x2c, 0x30, 0x34, 0x38};
    const uint32_t val[] = {l | 0x44u << 16 | 0x44u << 24, alpha, 0x20485699u, 0x00182261u, 0x82aa299cu, 0, 5, 0, 3, 0x9000, 0x34};
    uint32_t mode = 0;
    for (unsigned i = 0; i < sizeof off / sizeof off[0]; ++i)
        if (val[i] && io->write32(io->cookie, GPB_WIN_GPUCC, base + off[i], val[i])) return -1;
    if (gpb_rd(io, r, GPB_WIN_GPUCC, base + GPB_PLL_MODE, &mode)) return -1;
    if (io->write32(io->cookie, GPB_WIN_GPUCC, base + GPB_PLL_MODE, mode & ~GPB_PLL_OUTCTRL)) return -1;
    if (io->write32(io->cookie, GPB_WIN_GPUCC, base + GPB_PLL_OPMODE, 0)) return -1;      // PLL_STANDBY
    if (gpb_rd(io, r, GPB_WIN_GPUCC, base + GPB_PLL_MODE, &mode)) return -1;
    return io->write32(io->cookie, GPB_WIN_GPUCC, base + GPB_PLL_MODE, mode | GPB_PLL_RESET_N);
}
static inline int gpb_pll_running(uint32_t mode, uint32_t opmode) {return (opmode & GPB_PLL_RUN) && (mode & GPB_PLL_OUTCTRL);}
// 0 done, 1 timeout, -1 I/O.
static inline int gpb_rcg_set(const struct gpb_io *io, struct gpb_result *r, uint32_t rcgr, uint32_t sel, uint32_t div,
                              uint32_t *cfg_after, unsigned *polls) {
    uint32_t cmd = 0, cfg = 0;
    int timeout = 1;
    if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd) || io->write32(io->cookie, GPB_WIN_GPUCC, rcgr, cmd | GPB_RCG_ROOT_EN)) return -1;
    for (unsigned i = 0; i < 500; ++i) {
        if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd)) return -1;
        if (!(cmd & GPB_RCG_ROOT_OFF)) {timeout = 0; break;}
        if (io->delay_us) io->delay_us(io->cookie, 1);
    }
    if (timeout) return 1;
    if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr + 4, &cfg) ||
        io->write32(io->cookie, GPB_WIN_GPUCC, rcgr + 4, (cfg & ~GPB_RCG_CFG_CLEAR) | sel << 8 | div)) return -1;
    if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd) || io->write32(io->cookie, GPB_WIN_GPUCC, rcgr, cmd | GPB_RCG_UPDATE)) return -1;
    timeout = 1;
    for (unsigned i = 0; i < 500; ++i) {
        ++*polls;
        if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd)) return -1;
        if (!(cmd & GPB_RCG_UPDATE)) {timeout = 0; break;}
        if (io->delay_us) io->delay_us(io->cookie, 1);
    }
    if (timeout) return 1;
    if (io->write32(io->cookie, GPB_WIN_GPUCC, rcgr, cmd & ~GPB_RCG_ROOT_EN)) return -1;
    return gpb_rd(io, r, GPB_WIN_GPUCC, rcgr + 4, cfg_after) ? -1 : 0;
}
static inline int gpb_set_bit0(const struct gpb_io *io, struct gpb_result *r, unsigned w, uint32_t off) {
    uint32_t v = 0;
    if (gpb_rd(io, r, w, off, &v)) return -1;
    return (v & GPB_CBCR_ENABLE) ? 0 : io->write32(io->cookie, w, off, v | GPB_CBCR_ENABLE);
}
static inline void gpb_gmu_clocks_on(const struct gpb_io *io, struct gpb_result *r, const struct gpb_gmu_clocks *k,
                                     struct gpb_gmu_clock_on *o) {
    __builtin_memset(o, 0, sizeof *o);
    int e = 0;
    uint32_t v = 0;
    o->step = 1;
    if (!k->done || !gpb_gdsc_on(r->cx_gdscr)) {o->failed = 1; return;}
    if (gpb_rd(io, r, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &v)) goto io;
    if ((v & GPB_GPLL0_VOTES) != GPB_GPLL0_VOTES) {
        if (io->write32(io->cookie, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, v | GPB_GPLL0_VOTES)) goto io;
        if (io->delay_us) io->delay_us(io->cookie, 10);
    }
    o->step = 2;
    // A running PLL is left alone; a stopped one gets the probe's configuration.
    if (!gpb_pll_running(k->pll1_mode, k->pll1_opmode)) {
        uint32_t m0 = 0, op0 = 0;
        if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL0 + GPB_PLL_MODE, &m0) || gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL0 + GPB_PLL_OPMODE, &op0)) goto io;
        if (!gpb_pll_running(m0, op0) && gpb_pll_configure(io, r, GPB_CC_PLL0, 0x1e, 0xbaaa)) goto io;
        if (gpb_pll_configure(io, r, GPB_CC_PLL1, 0x16, 0xeaaa)) goto io;
        o->step = 3;
        if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_MODE, &v) ||
            io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_MODE, v | GPB_PLL_RESET_N) ||
            io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_OPMODE, GPB_PLL_RUN)) goto io;
        e = 1;
        for (unsigned i = 0; i < 1500; ++i) {
            ++o->lock_polls;
            if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_MODE, &v)) goto io;
            if (v & GPB_PLL_LOCK_DET) {e = 0; break;}
            if (io->delay_us) io->delay_us(io->cookie, 1);
        }
        if (e) {o->failed = 1; goto out;}
        if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_USER_CTL, &v) ||
            io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_USER_CTL, v | GPB_PLL_OUT_MASK) ||
            gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_MODE, &v) ||
            io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_MODE, v | GPB_PLL_OUTCTRL)) goto io;
    }
    o->step = 4;
    e = gpb_rcg_set(io, r, GPB_CC_HUB_RCGR, 5, 5, &o->hub_cfg, &o->hub_polls);
    if (e) {o->failed = e < 0 ? 2 : 1; goto out;}
    o->step = 5;
    e = gpb_rcg_set(io, r, GPB_CC_GMU_RCGR, 3, 3, &o->gmu_cfg, &o->gmu_polls);
    if (e) {o->failed = e < 0 ? 2 : 1; goto out;}
    o->step = 6;
    if (gpb_set_bit0(io, r, GPB_WIN_GPUCC, GPB_CC_CXO_AON_CLK) || gpb_set_bit0(io, r, GPB_WIN_GPUCC, GPB_CC_DEMET_CLK) ||
        gpb_set_bit0(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK) || gpb_set_bit0(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK) ||
        gpb_set_bit0(io, r, GPB_WIN_GPUCC, GPB_CC_CXO_CLK) || gpb_set_bit0(io, r, GPB_WIN_GCC_GPU, GPB_GCC_DDRSS_GPU_AXI) ||
        gpb_set_bit0(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MEMNOC_GFX) || gpb_set_bit0(io, r, GPB_WIN_GPUCC, GPB_CC_HUB_CX_INT_CLK)) goto io;
    e = 1;
    for (unsigned i = 0; i < 500; ++i) {
        if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, &o->cx_gmu)) goto io;
        if (gpb_clock_on(o->cx_gmu)) {e = 0; break;}
        if (io->delay_us) io->delay_us(io->cookie, 1);
    }
    if (e) {o->failed = 1; goto out;}
    o->step = 7;
    goto out;
io:
    o->failed = 2;
out:
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_MODE, &o->pll1_mode);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1 + GPB_PLL_OPMODE, &o->pll1_opmode);
}

// Formats the four status rows (47 characters each, space padded by the caller). Row text is the photo evidence.
static inline const char *gpb_state_text(enum gpb_state s) {
    return s == GPB_DONE ? "ok" : s == GPB_SKIPPED_PREREQ ? "skip" : s == GPB_SKIPPED_MODE ? "off" : s == GPB_NOMAP ? "nomap" : "-";
}
#endif
