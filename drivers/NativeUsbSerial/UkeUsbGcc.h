// SPDX-License-Identifier: BSD-2-Clause-Patent
// GCC/TCSR facts used by the uke USB2 warm handoff. Sources:
//   - Linux drivers/clk/qcom/gcc-cliffs.c (GPL-2.0), branch/reset definitions
//   - Linux drivers/clk/qcom/tcsrcc-pineapple.c (GPL-2.0), TCSR_USB2_CLKREF_EN
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

// tcsrcc@1f40000 (the uke DT uses compatible qcom,pineapple-tcsrcc),
// tcsr_usb2_clkref_en enable_reg/halt_reg 0xb1118, bit 0 — absolute
// 0x1fb1118. The earlier 0x1f4b118 was an arithmetic slip (0x1f40000 +
// 0xb1118 was mis-summed as 0x1f4b118): that page is the ADSP ipc_hwmutex
// region per the stock DT, so the write there was harmless and the read
// hung the tablet — and the USB2 reference-clock gate was never actually
// enabled, which is why the PHY's internal APB bridge stayed dead (usbbr4).
#define UKE_USB_TCSR_USB2_CLKREF_EN    0x01fb1118ULL
#define UKE_USB_TCSR_USB2_CLKREF_PAGE  0x01fb1000ULL
#define UKE_USB_TCSR_USB2_CLKREF_OFF   0x118U

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
