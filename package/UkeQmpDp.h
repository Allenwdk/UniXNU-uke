// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include "UkeDpLinkPolicy.h"

// Uke 4nm-v1.1 DP profile from Senemos fd33b9174872, 0060-phy-qcom-uke-gen1-dp-profile.patch.
// DP rate and TX tables use Linux v7.2 phy-qcom-qmp-combo.c and its v6 QSERDES offsets.
struct UkeDpPhyWrite {uint16_t offset; uint8_t value;};

static constexpr UkeDpPhyWrite kUkeDpSerdes[] = {
    {0x17c, 0x15}, // COM_SVS_MODE_CLK_SEL
    {0x110, 0x3b}, // COM_SYSCLK_EN_SEL
    {0x0e4, 0x02}, // COM_SYS_CLK_CTRL
    {0x0e0, 0x0c}, // COM_CLK_ENABLE1
    {0x0e8, 0x06}, // COM_SYSCLK_BUF_ENABLE
    {0x164, 0x30}, // COM_CLK_SELECT
    {0x0f4, 0x0f}, // COM_PLL_IVCO for Xiaomi's DP PLL 4nm v1.1.
    {0x078, 0x36}, // COM_PLL_CCTRL_MODE0
    {0x074, 0x16}, // COM_PLL_RCTRL_MODE0
    {0x070, 0x06}, // COM_CP_CTRL_MODE0
    {0x090, 0x00}, // COM_DIV_FRAC_START1_MODE0
    {0x174, 0x12}, // COM_CMN_CONFIG_1
    {0x0a0, 0x3f}, // COM_INTEGLOOP_GAIN0_MODE0
    {0x0a4, 0x00}, // COM_INTEGLOOP_GAIN1_MODE0
    {0x140, 0x00}, // COM_VCO_TUNE_MAP
    {0x0bc, 0x0a}, // COM_BG_TIMER
    {0x07c, 0x14}, // COM_PLL_CORE_CLK_DIV_MODE0
    {0x13c, 0x00}, // COM_VCO_TUNE_CTRL
    {0x0dc, 0x17}, // COM_PLL_BIAS_EN_CLK_BUFLR_EN
    {0x170, 0x0f}, // COM_CORE_CLK_EN
};

static constexpr UkeDpPhyWrite kUkeDpRbr[] = {
    {0x03c, 0x05}, // COM_HSCLK_SEL_1
    {0x088, 0x34}, // COM_DEC_START_MODE0
    {0x094, 0xc0}, // COM_DIV_FRAC_START2_MODE0
    {0x098, 0x0b}, // COM_DIV_FRAC_START3_MODE0
    {0x080, 0x37}, // COM_LOCK_CMP1_MODE0
    {0x084, 0x04}, // COM_LOCK_CMP2_MODE0
    {0x120, 0x04}, // COM_LOCK_CMP_EN
    {0x058, 0x71}, // COM_BIN_VCOCAL_CMP_CODE1_MODE0
    {0x05c, 0x0c}, // COM_BIN_VCOCAL_CMP_CODE2_MODE0
};

static constexpr UkeDpPhyWrite kUkeDpHbr[] = {
    {0x03c, 0x03}, // COM_HSCLK_SEL_1
    {0x088, 0x34}, // COM_DEC_START_MODE0
    {0x094, 0xc0}, // COM_DIV_FRAC_START2_MODE0
    {0x098, 0x0b}, // COM_DIV_FRAC_START3_MODE0
    {0x080, 0x07}, // COM_LOCK_CMP1_MODE0
    {0x084, 0x07}, // COM_LOCK_CMP2_MODE0
    {0x120, 0x08}, // COM_LOCK_CMP_EN
    {0x058, 0x71}, // COM_BIN_VCOCAL_CMP_CODE1_MODE0
    {0x05c, 0x0c}, // COM_BIN_VCOCAL_CMP_CODE2_MODE0
};

// Linux QSERDES v6 HBR2 table also matches Xiaomi's DP PLL 4nm v1.1.
static constexpr UkeDpPhyWrite kUkeDpHbr2[] = {
    {0x03c, 0x01}, // COM_HSCLK_SEL_1
    {0x088, 0x46}, // COM_DEC_START_MODE0
    {0x094, 0x00}, // COM_DIV_FRAC_START2_MODE0
    {0x098, 0x05}, // COM_DIV_FRAC_START3_MODE0
    {0x080, 0x0f}, // COM_LOCK_CMP1_MODE0
    {0x084, 0x0e}, // COM_LOCK_CMP2_MODE0
    {0x120, 0x08}, // COM_LOCK_CMP_EN
    {0x058, 0x97}, // COM_BIN_VCOCAL_CMP_CODE1_MODE0
    {0x05c, 0x10}, // COM_BIN_VCOCAL_CMP_CODE2_MODE0
};

constexpr unsigned ukeDpPhyVcoDividerCode(uint8_t rate) {
    return rate == UkeDpLink::kHbr2 ? 2 : 1;
}

static constexpr UkeDpPhyWrite kUkeDpTx[] = {
    {0x0c8, 0x40}, // TX_VMODE_CTRL1
    {0x020, 0x30}, // TX_PRE_STALL_LDO_BOOST_EN
    {0x02c, 0x3b}, // TX_INTERFACE_SELECT
    {0x008, 0x0f}, // TX_CLKBUF_ENABLE
    {0x01c, 0x03}, // TX_RESET_TSYNC_EN
    {0x0c0, 0x0f}, // TX_TRAN_DRVR_EMP_EN
    {0x060, 0x00}, // TX_PARRATE_REC_DETECT_IDLE_EN
    {0x0c4, 0x00}, // TX_TX_INTERFACE_MODE
    {0x03c, 0x0c}, // TX_RES_CODE_LANE_OFFSET_TX
    {0x040, 0x0c}, // TX_RES_CODE_LANE_OFFSET_RX
    {0x024, 0x04}, // TX_TX_BAND
};

static constexpr uint8_t kUkeDpSwing[4][4] = {
    { 0x07, 0x0f, 0x16, 0x1f },
    { 0x11, 0x1e, 0x1f, 0xff },
    { 0x16, 0x1f, 0xff, 0xff },
    { 0x1f, 0xff, 0xff, 0xff },
};

static constexpr uint8_t kUkeDpPreEmphasis[4][4] = {
    { 0x00, 0x0d, 0x14, 0x1a },
    { 0x00, 0x0e, 0x15, 0xff },
    { 0x00, 0x0e, 0xff, 0xff },
    { 0x02, 0xff, 0xff, 0xff },
};

// Uke OEM DT DP0 qcom,hbr2-3-{voltage-swing,pre-emphasis}.
static constexpr uint8_t kUkeDpHbr2Swing[4][4] = {
    { 0x02, 0x12, 0x16, 0x1a },
    { 0x09, 0x19, 0x1f, 0xff },
    { 0x10, 0x1f, 0xff, 0xff },
    { 0x1f, 0xff, 0xff, 0xff },
};

static constexpr uint8_t kUkeDpHbr2PreEmphasis[4][4] = {
    { 0x00, 0x0c, 0x15, 0x1b },
    { 0x02, 0x0e, 0x16, 0xff },
    { 0x02, 0x11, 0xff, 0xff },
    { 0x04, 0xff, 0xff, 0xff },
};
