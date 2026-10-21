// SPDX-License-Identifier: BSD-2-Clause-Patent
// GCC/TCSR facts used by the uke USB2 warm handoff. Sources:
//   - Linux drivers/clk/qcom/gcc-cliffs.c (GPL-2.0), branch/reset definitions
//   - Linux drivers/clk/qcom/tcsrcc-uke.c (GPL-2.0), TCSR_USB2_CLKREF_EN
//   - Linux drivers/clk/qcom/gdsc-regulator-uke.c (GPL-2.0), GDSCR bits
//   - stock uke DT clock/reset phandles and the uke-mainline USB2 overlay
// The driver writes only branch gates, the two resets and the GDSC; it does not
// reprogram the RCG rate sources. ABL/UEFI is expected to have selected the
// core clock rate (the OEM DT documents 133/200/66.6 MHz rates).
#ifndef UKE_USB_GCC_H
#define UKE_USB_GCC_H

#include "UkeUsbBoard.h"

// GCC_USB30_PRIM_BCR (reset ID 19): bit 0, 1 = assert, 0 = deassert.
#define UKE_USB_GCC_USB30_BCR          (UKE_USB_GCC_BASE + 0x39000ULL)
// GCC_QUSB2PHY_PRIM_BCR (reset ID 15): eUSB2 PHY reset.
#define UKE_USB_GCC_QUSB2PHY_BCR       (UKE_USB_GCC_BASE + 0x12000ULL)

// clk_branch enable registers, all bit 0 (gcc-cliffs.c).
#define UKE_USB_GCC_MASTER_CLK_EN      (UKE_USB_GCC_BASE + 0x39018ULL) // core_clk
#define UKE_USB_GCC_SLEEP_CLK_EN       (UKE_USB_GCC_BASE + 0x39024ULL) // sleep_clk
#define UKE_USB_GCC_UTMI_CLK_EN        (UKE_USB_GCC_BASE + 0x39028ULL) // utmi_clk
#define UKE_USB_GCC_CFG_NOC_CLK_EN     (UKE_USB_GCC_BASE + 0x39088ULL) // iface_clk
#define UKE_USB_GCC_AGGRE_CLK_EN       (UKE_USB_GCC_BASE + 0x3908cULL) // bus_aggr_clk

// tcsrcc@1f40000, tcsr_usb2_clkref_en at 0xb1118, bit 0.
#define UKE_USB_TCSR_USB2_CLKREF_EN    0x01f4b118ULL

#define UKE_USB_GCC_BRANCH_ENABLE      0x00000001U
#define UKE_USB_GCC_RESET_ASSERT       0x00000001U

// GDSCR bits (gdsc-regulator-uke.c).
#define UKE_USB_GDSCR_SW_COLLAPSE      (1U << 0)
#define UKE_USB_GDSCR_HW_CONTROL       (1U << 1)
#define UKE_USB_GDSCR_SW_OVERRIDE      (1U << 2)
#define UKE_USB_GDSCR_RETAIN_FF        (1U << 11)
#define UKE_USB_GDSCR_CFG_POWER_DOWN   (1U << 15)
#define UKE_USB_GDSCR_CFG_POWER_UP     (1U << 16)
#define UKE_USB_GDSC_TIMEOUT_US        1500U

#endif /* UKE_USB_GCC_H */
