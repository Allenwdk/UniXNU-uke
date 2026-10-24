// SPDX-License-Identifier: BSD-2-Clause-Patent
// eUSB2 PHY facts for the uke USB2 peripheral. Sources:
//   - Linux drivers/usb/phy/phy-msm-snps-eusb2.c (GPL-2.0-only), the OEM
//     register sequence and the 19.2/38.4 MHz FSEL/PLL divider pairs
//   - stock uke DT hsphy@88e3000 (param-override-seq = <0x00 0x58>)
// The reference clock is 38.4 MHz, not the 19.2 MHz mainline boards use:
// the stock DT feeds the PHY rpmhcc CXO_PAD_CLK (index 0x1b), which is the
// ARC "xo.lvl" with div 2 under the fixed 76.8 MHz xo_board — 38.4 MHz.
// The 19.2 MHz table kept the PLL from ever locking and left the core's
// device domain without its functional clock (usbphy1..usbbr2).
// Values are register facts, not copied driver code. The .cpp applies each
// write with readback.
#ifndef UKE_USB_PHY_H
#define UKE_USB_PHY_H

#include <stdint.h>
#include <stddef.h>

// Register offsets inside hsphy@88e3000.
#define UKE_USB_PHY_UTMI_CTRL0          0x03cU
#define UKE_USB_PHY_UTMI_CTRL5          0x050U
#define UKE_USB_PHY_COMMON0             0x054U
#define UKE_USB_PHY_CFG_CTRL_1          0x058U
#define UKE_USB_PHY_CFG_CTRL_2          0x05cU
#define UKE_USB_PHY_CFG_CTRL_3          0x060U
#define UKE_USB_PHY_HS_PHY_CTRL2        0x064U
#define UKE_USB_PHY_CFG_CTRL_4          0x068U
#define UKE_USB_PHY_CFG_CTRL_5          0x06cU
#define UKE_USB_PHY_CFG_CTRL_6          0x070U
#define UKE_USB_PHY_CFG_CTRL_8          0x078U
#define UKE_USB_PHY_CFG_CTRL_9          0x07cU
#define UKE_USB_PHY_CFG0                0x094U
#define UKE_USB_PHY_UTMI_PHY_CMN_CTRL0  0x098U
#define UKE_USB_PHY_FSEL_SEL            0x0b8U
#define UKE_USB_PHY_APB_ACCESS_CMD      0x130U

// COMMON0 bits.
#define UKE_USB_PHY_COMMON0_PHY_ENABLE  (1U << 0)
#define UKE_USB_PHY_COMMON0_SIDDQ_SEL   (1U << 1)
#define UKE_USB_PHY_COMMON0_SIDDQ       (1U << 2)
#define UKE_USB_PHY_COMMON0_RETENABLEN  (1U << 3)
#define UKE_USB_PHY_COMMON0_FSEL_38_4M  (4U << 4)
#define UKE_USB_PHY_COMMON0_FSEL_MASK   (0x7U << 4)

// HS_PHY_CTRL2 bits.
#define UKE_USB_PHY_HS2_VBUS_DET_EXT_SEL (1U << 4)
#define UKE_USB_PHY_HS2_SUSPEND_N_SEL    (1U << 3)
#define UKE_USB_PHY_HS2_SUSPEND_N        (1U << 2)

// PHY_CFG0 / UTMI_CTRL5 / APB bits.
#define UKE_USB_PHY_CFG0_CMN_OVERRIDE    (1U << 1)
#define UKE_USB_PHY_UTMI5_POR            (1U << 1)
#define UKE_USB_PHY_APB_LOGIC_RESET      (1U << 2)

struct UkeUsbPhyWrite {
	uint16_t offset;
	uint32_t value;
	uint32_t mask;
};

// Sequence for the 38.4 MHz reference clock. Each entry is a masked write; a
// zero mask bit leaves the register bit unchanged.
static const UkeUsbPhyWrite kUkeUsbPhyInit38_4Mhz[] = {
	// Common block override, power-on reset, PHY enable and retention.
	{ UKE_USB_PHY_CFG0,               UKE_USB_PHY_CFG0_CMN_OVERRIDE, UKE_USB_PHY_CFG0_CMN_OVERRIDE },
	{ UKE_USB_PHY_UTMI_CTRL5,         UKE_USB_PHY_UTMI5_POR,        UKE_USB_PHY_UTMI5_POR },
	{ UKE_USB_PHY_COMMON0,            UKE_USB_PHY_COMMON0_PHY_ENABLE | UKE_USB_PHY_COMMON0_RETENABLEN,
	                                  UKE_USB_PHY_COMMON0_PHY_ENABLE | UKE_USB_PHY_COMMON0_RETENABLEN },
	{ UKE_USB_PHY_APB_ACCESS_CMD,     UKE_USB_PHY_APB_LOGIC_RESET,  UKE_USB_PHY_APB_LOGIC_RESET },
	{ UKE_USB_PHY_UTMI_PHY_CMN_CTRL0, 0U,                           (1U << 6) },
	{ UKE_USB_PHY_FSEL_SEL,           1U,                           1U },
	// 38.4 MHz FSEL and PLL dividers (FSEL_38_4_MHZ_VAL, DIV_7_0_38_4,
	// DIV_11_8_38_4 in the vendor driver).
	{ UKE_USB_PHY_COMMON0,            UKE_USB_PHY_COMMON0_FSEL_38_4M, UKE_USB_PHY_COMMON0_FSEL_MASK },
	{ UKE_USB_PHY_CFG_CTRL_2,         0xc8U,                        0xffU },
	{ UKE_USB_PHY_CFG_CTRL_3,         0x00U,                        0xffU },
	{ UKE_USB_PHY_CFG_CTRL_1,         1U << 1,                      0xfeU },
	{ UKE_USB_PHY_CFG_CTRL_4,         (8U << 2) | 1U,               0xffU },
	{ UKE_USB_PHY_CFG_CTRL_5,         0x10U | (1U << 6),            0xffU },
	{ UKE_USB_PHY_CFG_CTRL_6,         0U,                           0x07U },
	{ UKE_USB_PHY_HS_PHY_CTRL2,       UKE_USB_PHY_HS2_VBUS_DET_EXT_SEL, UKE_USB_PHY_HS2_VBUS_DET_EXT_SEL },
	{ UKE_USB_PHY_CFG_CTRL_9,         (1U << 3) | (2U << 5),        0x7fU },
	{ UKE_USB_PHY_CFG_CTRL_8,         3U << 3,                      0xf8U },
	// DT param-override-seq <0x00 0x58>.
	{ UKE_USB_PHY_CFG_CTRL_1,         0U,                           0xffU },
	// Finish the power-on sequence.
	{ UKE_USB_PHY_HS_PHY_CTRL2,       UKE_USB_PHY_HS2_SUSPEND_N_SEL | UKE_USB_PHY_HS2_SUSPEND_N,
	                                  UKE_USB_PHY_HS2_SUSPEND_N_SEL | UKE_USB_PHY_HS2_SUSPEND_N },
	{ UKE_USB_PHY_UTMI_CTRL0,         1U,                           1U },
	{ UKE_USB_PHY_COMMON0,            UKE_USB_PHY_COMMON0_SIDDQ_SEL, UKE_USB_PHY_COMMON0_SIDDQ_SEL },
	{ UKE_USB_PHY_COMMON0,            0U,                           UKE_USB_PHY_COMMON0_SIDDQ },
	{ UKE_USB_PHY_UTMI_CTRL5,         0U,                           UKE_USB_PHY_UTMI5_POR },
	{ UKE_USB_PHY_HS_PHY_CTRL2,       0U,                           UKE_USB_PHY_HS2_SUSPEND_N_SEL },
	{ UKE_USB_PHY_CFG0,               0U,                           UKE_USB_PHY_CFG0_CMN_OVERRIDE },
};

#define UKE_USB_PHY_INIT_COUNT (sizeof(kUkeUsbPhyInit38_4Mhz) / sizeof(kUkeUsbPhyInit38_4Mhz[0]))

#endif /* UKE_USB_PHY_H */
