// SPDX-License-Identifier: BSD-2-Clause-Patent
// Physical facts for the uke SM7675 USB2 peripheral. Sources:
//   - /home/allenwdk/uke-port/data/uke-live.dts (stock QCOM DT)
//   - uke-mainline src/boot/sm7675-uke-usb2.dtso (GPL-2.0-only OR BSD-2-Clause)
//   - Linux gcc-cliffs.c / tcsrcc-uke.c / gdsc-regulator-uke.c (GPL-2.0)
// Only addresses, offsets and bit values are transcribed here; no driver code
// is copied. The USB wrapper is outside the XNU arm-io window (0x17000000), so
// the driver maps these physical windows directly, like NativeUfs does.
#ifndef UKE_USB_BOARD_H
#define UKE_USB_BOARD_H

#define UKE_USB_WRAPPER_BASE        0x0a600000ULL
#define UKE_USB_WRAPPER_SIZE        0x00100000ULL
#define UKE_USB_CORE_BASE           0x0a600000ULL
#define UKE_USB_CORE_SIZE           0x0000d93cULL

// QSCRATCH is wrapper base + 0xf8800: dwc3-qcom.c maps
// SDM845_QSCRATCH_BASE_OFFSET (0xf8800) for SDM845_QSCRATCH_SIZE (0x400)
// bytes. The stock uke DT gives usb0 one 1 MiB window at 0xa600000, so the
// page sits inside it.
#define UKE_USB_QSCRATCH_BASE       (UKE_USB_WRAPPER_BASE + 0xf8800ULL)
#define UKE_USB_QSCRATCH_SIZE       0x00000400ULL
// GENERAL_CFG (0x08) routes the clock the device domain runs on. ABL leaves
// PIPE_UTMI_CLK_SEL (bit 0: feed the pipe-clock input from the UTMI clock —
// this board has no PIPE3) and PIPE3_PHYSTATUS_SW (bit 3) set; the PHY BCR
// pulse resets the glue and drops both, which starves the command engine.
#define UKE_USB_QSCRATCH_GENERAL_CFG       0x08U
#define UKE_USB_QSCRATCH_PIPE_UTMI_CLK_SEL  (1U << 0)
#define UKE_USB_QSCRATCH_PIPE3_PHYSTATUS_SW (1U << 3)
#define UKE_USB_QSCRATCH_PIPE_UTMI_CLK_DIS  (1U << 8)
// The value the first revision transcribed. It is outside the wrapper window
// and raises a synchronous external abort; kept only to reproduce that.
#define UKE_USB_QSCRATCH_BASE_BAD   0x0b58800ULL

// GCC window. The USB GDSC is the gcc_usb30_prim_gdsc at 0x139004.
#define UKE_USB_GCC_BASE            0x00100000ULL
#define UKE_USB_GDSC_BASE           0x00139000ULL
#define UKE_USB_GDSC_SIZE           0x00001000ULL
#define UKE_USB_GDSC_REG_OFFSET     0x00000004U
#define UKE_USB_GDSC_CFG_OFFSET     0x00000008U

// eUSB2 PHY and EUD registers from the stock DT.
#define UKE_USB_PHY_BASE            0x088e3000ULL
#define UKE_USB_PHY_SIZE            0x00000154ULL
#define UKE_USB_EUD_ENABLE_BASE     0x088e2000ULL
#define UKE_USB_EUD_DETECT_BASE     0x0c278000ULL

// apps_smmu@15000000; the DWC3 stream is 0x40.
#define UKE_USB_SMMU_BASE           0x15000000ULL
#define UKE_USB_SMMU_SIZE           0x000d0000ULL
#define UKE_USB_STREAM_ID           0x0040U

// DWC3 core IRQ (DT <0 133 4>, level high).
#define UKE_USB_IRQ                 133U

// eUSB2 reference clock from the DT (19.2 MHz).
#define UKE_USB_PHY_REF_HZ          19200000U

// Warm-handoff bus targets. Not touched by the first probe/warm driver; kept
// here for the cold-init follow-up and for documentation.
#define UKE_USB_WCD_I2C_BASE        0x0a80000ULL
#define UKE_USB_WCD_I2C_ADDR        0x0eU
#define UKE_USB_WCD_RESET_GPIO      119U
#define UKE_USB_REPEATER_SPMI_ADDR  0xfd00U

#endif /* UKE_USB_BOARD_H */
