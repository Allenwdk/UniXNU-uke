// SPDX-License-Identifier: BSD-2-Clause-Patent
// uke (Cliffs) touch controller board facts and the pure helpers around them. Every constant states its source.
// Nothing here touches hardware; TouchProbe.cpp owns the MMIO. Host-tested by TouchBoardTests.c.
#ifndef UKE_TOUCH_BOARD_H
#define UKE_TOUCH_BOARD_H
#include <stdint.h>

// Board DT (uke-port/data/uke-live.dts, collected from the tablet): qcom,qupv3_0_geni_se@ac0000 wraps
// qupv3_se4_spi: spi@a90000 (reg <0xa90000 0x4000>) with the NT36532 at novatek@0, spi-max-frequency 19200000, mode 0.
// Unlike sheng (SM8550) this is QUP wrap 0, so every GCC branch and the SE clock source are different registers.
#define TB_QUP_WRAPPER_BASE 0x00ac0000ULL
#define TB_SE_BASE 0x00a90000ULL
#define TB_SE_SIZE 0x4000ULL
// uke-live.dts qupv3_0_geni_se@ac0000: iommus = <0x129 0xa3 0x00>; apps-smmu@15000000 reg <0x15000000 0x100000>.
#define TB_QUP_SMMU_SID 0xa3
#define TB_APPS_SMMU_BASE 0x15000000ULL
#define TB_APPS_SMMU_SIZE 0x100000ULL
// clock-controller@100000 (compatible qcom,cliffs-gcc); branch/RCG offsets from Xiaomi_Kernel_OpenSource-uke gcc-cliffs.c.
#define TB_GCC_BASE 0x00100000ULL
#define TB_GCC_VOTE_ENABLE 0x52008            // enable_reg of every QUP wrap0 branch below
#define TB_GCC_GPLL0_ENABLE 0x52020           // gcc_gpll0 enable_reg, bit 0; a fixed Lucid OLE PLL, always on
#define TB_VOTE_WRAP0_CORE_2X (1u<<18)        // gcc_qupv3_wrap0_core_2x_clk, halt 0x23018
#define TB_VOTE_WRAP0_CORE (1u<<19)           // gcc_qupv3_wrap0_core_clk, halt 0x23008
#define TB_VOTE_WRAP0_M_AHB (1u<<20)          // gcc_qupv3_wrap_0_m_ahb_clk, halt 0x23000, hwcg bit 1
#define TB_VOTE_WRAP0_S_AHB (1u<<21)          // gcc_qupv3_wrap_0_s_ahb_clk, halt 0x23004, hwcg bit 1
#define TB_VOTE_WRAP0_S4 (1u<<26)             // gcc_qupv3_wrap0_s4_clk, halt 0x183bc
#define TB_VOTE_ALL (TB_VOTE_WRAP0_CORE_2X|TB_VOTE_WRAP0_CORE|TB_VOTE_WRAP0_M_AHB|TB_VOTE_WRAP0_S_AHB|TB_VOTE_WRAP0_S4)
#define TB_HALT_CORE_2X 0x23018
#define TB_HALT_CORE 0x23008
#define TB_HALT_M_AHB 0x23000
#define TB_HALT_S_AHB 0x23004
#define TB_HALT_S4 0x183bc
#define TB_CBCR_CLK_OFF (1u<<31)
// gcc_qupv3_wrap0_s4_clk_src: cmd_rcgr 0x183c8, a plain freq_tbl RCG (clk_rcg2_ops; NOT DFS-managed like sheng's
// wrap1 s4), mnd_width 16, hid_width 5, parents gcc_parent_map_0 = { TCXO 0, GPLL0 main 1, GPLL0 even 6 }. The probe
// programs the F(19200000, P_BI_TCXO, 1, 0, 0) entry itself, so the SE source is TCXO undivided with no GPLL0 dependency.
#define TB_S4_RCG 0x183c8
#define TB_RCG_CMD 0x0                        // bit 0 CFG_UPDATE, self-clearing once the config is applied
#define TB_RCG_CFG 0x4                        // src 10:8, mode 13:12, half-integer hid divider 4:0
#define TB_RCG_M 0x8
#define TB_RCG_N 0xc                          // stored as ~(n-m) (clk_rcg2_configure)
#define TB_RCG_D 0x10
#define TB_RCG_SRC_TCXO 0
#define TB_RCG_SRC_GPLL0 1
#define TB_RCG_SRC_GPLL0_EVEN 6
#define TB_SCLK_HZ 19200000ULL                // the SE source clock the probe programs (TCXO / 1)
// TLMM (uke-live.dts pinctrl@f000000, compatible qcom,cliffs-pinctrl; pinctrl-cliffs.c: REG_BASE = 0x100000 inside the node's
// reg window, so the per-GPIO block starts at 0x0f000000 + 0x100000; ctl = + 0x1000*gpio, io = +4, mux_bit 2, drv_bit 6;
// function slot 0 is gpio, so each qup0_se4 lane is slot 1 in its own pingroup).
#define TB_TLMM_BASE 0x0f100000ULL
#define TB_GPIO_STRIDE 0x1000
#define TB_GPIO_SPI_MISO 16                   // qupv3_se4_spi_miso_active: gpio16, qup0_se4_l0
#define TB_GPIO_SPI_MOSI 17                   // qupv3_se4_spi_mosi_active: gpio17, qup0_se4_l1
#define TB_GPIO_SPI_CLK 18                    // mi_ts_spi_clk_active: gpio18, qup0_se4_l2
#define TB_GPIO_SPI_CS 19                     // mi_ts_spi_cs_active: gpio19, qup0_se4_l3
#define TB_GPIO_IRQ 54                        // novatek,irq-gpio = <&tlmm 54 0x2001>
#define TB_FUNC_QUP0_SE4 1
#define TB_DRIVE_6MA 2                        // drive-strength = <6>: (mA/2)-1
#define TB_SPI_MODE 0                         // touchscreen@0: spi-max-frequency 19200000, mode 0
#define TB_SPI_DEFAULT_HZ 19200000u
#define TB_SPI_MAX_HZ 19200000u
#define TB_TOUCH_AXIS_MAX_Y 20319             // NT36532 report units (0.1 px); same 60x40 matrix as sheng until a verdict
// The touch interrupt: a TLMM pin interrupt on GPIO 54 summarised by the TLMM SPI (pinctrl@f000000
// interrupts = <0 0xd0 4> = GIC SPI 208, level high), INTID 240.
#define TB_TLMM_SPI 208
#define TB_TLMM_INTID (32u + TB_TLMM_SPI)
// INTR_CFG target field: pinctrl-cliffs.c PINGROUP intr_target_bit = 8, intr_target_kpss_val = 3.
// sheng/Kalama keeps the same value at bit 5; copying that shift leaves the uke target unwritten.
#define TB_TLMM_INTR_TARGET_BIT 8
#define TB_TLMM_INTR_TARGET_APPS 3

static inline uint32_t tb_pin_cfg(unsigned func, unsigned drive_code) {return (func&15u)<<2|(drive_code&7u)<<6;}   // pull disabled
static inline uint32_t tb_tlmm_ctl(unsigned gpio) {return (uint32_t)gpio*TB_GPIO_STRIDE;}
static inline uint32_t tb_tlmm_io(unsigned gpio) {return (uint32_t)gpio*TB_GPIO_STRIDE+4;}

// Source clock rates by RCG source-select value (gcc-cliffs.c gcc_parent_map_0). GPLL0 main is 600 MHz (the wrap0
// freq_tbl derives 100 MHz from main/6 and 120 MHz from main/5) and even is its half output, 300 MHz (75 MHz = even/4).
static inline uint64_t tb_src_hz(unsigned sel) {return sel==0?19200000ULL:sel==1?600000000ULL:sel==6?300000000ULL:0;}
// clk-rcg2.c calc_rate: a nonzero hid field divides by (hid+1)/2 (half-integer divider), then M/N.
static inline uint64_t tb_calc_rate(uint64_t prate, uint32_t m, uint32_t n, unsigned mode, unsigned hid) {
    if (hid) {prate*=2;prate/=hid+1;}
    if (mode && n) {prate*=m;prate/=n;}
    return prate;
}
// Frequency of the RCG from its CFG (src 10:8, mode 13:12, hid 4:0) and M/N registers (mnd_width 16; N stores ~(n-m)).
static inline uint64_t tb_rcg_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src=(cfg>>8)&7,mode=(cfg>>12)&3,hid=cfg&0x1f;
    uint32_t m=0,n=0;
    if (mode) {m=mreg&0xffff;n=((~nreg)&0xffff)+m;}
    return tb_calc_rate(tb_src_hz(src),m,n,mode,hid);
}
// Encode the CFG word for an integer divider d >= 1 (the F macro writes hid = 2*d-1) and a 0/2 mode.
static inline uint32_t tb_rcg_cfg(unsigned src, unsigned div, unsigned mode) {return ((src&7u)<<8)|((2u*div-1u)&0x1fu)|((mode&3u)<<12);}
static inline uint32_t tb_rcg_n(unsigned n, unsigned m) {return (uint32_t)(~(n-m))&0xffffu;}
#endif
