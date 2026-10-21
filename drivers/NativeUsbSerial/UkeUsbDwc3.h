// SPDX-License-Identifier: BSD-2-Clause-Patent
// DWC3 register/TRB/event facts for the uke USB2 peripheral. Sources:
//   - Synopsys DWC3 databook (register and TRB formats)
//   - Linux drivers/usb/dwc3/core.h, gadget.h, gadget.c (GPL-2.0) as a
//     cross-check for the published bit positions
// Only constants and wire formats are transcribed; the XNU driver implements
// its own controller logic.
#ifndef UKE_USB_DWC3_H
#define UKE_USB_DWC3_H

#include <stdint.h>
#include <stddef.h>
#include "UkeUsbBoard.h"

// Global registers.
#define UKE_USB_DWC3_GCTL            0xc110U
#define UKE_USB_DWC3_GSTS            0xc118U
#define UKE_USB_DWC3_GSNPSID         0xc120U
#define UKE_USB_DWC3_GUID            0xc128U
#define UKE_USB_DWC3_GHWPARAMS0      0xc140U
#define UKE_USB_DWC3_GHWPARAMS1      0xc144U
#define UKE_USB_DWC3_GHWPARAMS2      0xc148U
#define UKE_USB_DWC3_GHWPARAMS3      0xc14cU
#define UKE_USB_DWC3_GUSB2PHYCFG(n)  (0xc200U + 4U * (n))
#define UKE_USB_DWC3_GUSB3PIPECTL(n) (0xc2c0U + 4U * (n))
#define UKE_USB_DWC3_GTXFIFOSIZ(n)   (0xc300U + 4U * (n))
#define UKE_USB_DWC3_GRXFIFOSIZ(n)   (0xc380U + 4U * (n))
#define UKE_USB_DWC3_GEVNTADRLO      0xc400U
#define UKE_USB_DWC3_GEVNTADRHI      0xc404U
#define UKE_USB_DWC3_GEVNTSIZ        0xc408U
#define UKE_USB_DWC3_GEVNTCOUNT      0xc40cU
#define UKE_USB_DWC3_DCFG            0xc700U
#define UKE_USB_DWC3_DCTL            0xc704U
#define UKE_USB_DWC3_DEVTEN          0xc708U
#define UKE_USB_DWC3_DSTS            0xc70cU
#define UKE_USB_DWC3_DEPCMDPAR0(n)   (0xc720U + 16U * (n))
#define UKE_USB_DWC3_DEPCMDPAR1(n)   (0xc724U + 16U * (n))
#define UKE_USB_DWC3_DEPCMDPAR2(n)   (0xc728U + 16U * (n))
#define UKE_USB_DWC3_DEPCMD(n)       (0xc72cU + 16U * (n))

// GCTL.
#define UKE_USB_DWC3_GCTL_DSBLCLKGTNG   (1U << 0)

// GUSB2PHYCFG.
#define UKE_USB_DWC3_GUSB2PHYCFG_SUSPHY   (1U << 6)
#define UKE_USB_DWC3_GUSB2PHYCFG_ENBLSLPM (1U << 8)

// GUSB3PIPECTL.
#define UKE_USB_DWC3_GUSB3PIPECTL_SUSPHY  (1U << 17)
#define UKE_USB_DWC3_GUSB3PIPECTL_DEP1P2P3_EN (1U << 19)

// DCFG/DCTL/DSTS.
#define UKE_USB_DWC3_DCFG_DEVSPD_HS   0U
#define UKE_USB_DWC3_DCFG_DEVADDR(n)  (((n) & 0x7fU) << 3)
#define UKE_USB_DWC3_DCTL_RUN_STOP    (1U << 31)
#define UKE_USB_DWC3_DCTL_CSFTRST     (1U << 30)
#define UKE_USB_DWC3_DSTS_DEVCTRLHLT  (1U << 22)
#define UKE_USB_DWC3_DSTS_CONNECTSPD  (0x7U << 0)

// DEVTEN.
#define UKE_USB_DWC3_DEVTEN_DISCONN   (1U << 0)
#define UKE_USB_DWC3_DEVTEN_USBRST    (1U << 1)
#define UKE_USB_DWC3_DEVTEN_CONNECTDONE (1U << 2)
#define UKE_USB_DWC3_DEVTEN_ULSTCNG   (1U << 3)
#define UKE_USB_DWC3_DEVTEN_WAKEUP    (1U << 4)
#define UKE_USB_DWC3_DEVTEN_SOF       (1U << 7)
#define UKE_USB_DWC3_DEVTEN_ERRTIC    (1U << 9)
#define UKE_USB_DWC3_DEVTEN_CMDCMPLT  (1U << 10)

// Event-buffer entry: four 32-bit words.
struct UkeUsbEvent {
	uint32_t info;
	uint32_t param;
	uint32_t status;
	uint32_t reserved;
} __attribute__((packed));

// TRB entry: four 32-bit words.
struct UkeUsbTrb {
	uint32_t bpl;
	uint32_t bph;
	uint32_t size;
	uint32_t ctrl;
} __attribute__((packed));

static_assert(sizeof(UkeUsbEvent) == 16, "DWC3 event entry must be 16 bytes");
static_assert(sizeof(UkeUsbTrb) == 16, "DWC3 TRB must be 16 bytes");

// Event raw word type. Bit 0 is is_devspec/one_bit: 0 = device event,
// 1 = endpoint event.
#define UKE_USB_DWC3_EVENT_IS_ENDPOINT(info)  (((info) & 1U) != 0)
#define UKE_USB_DWC3_DEVICE_EVENT(info)      (((info) >> 1) & 0x7fU)
#define UKE_USB_DWC3_EP_NUMBER(info)         (((info) >> 1) & 0x1fU)
#define UKE_USB_DWC3_EP_EVENT(info)          (((info) >> 6) & 0x0fU)
#define UKE_USB_DWC3_EP_STATUS(info)         (((info) >> 10) & 0x0fU)
#define UKE_USB_DWC3_EP_PARAM(info)          (((info) >> 14) & 0xffffU)

// Device event codes.
#define UKE_USB_DWC3_DEV_DISCONNECT      0U
#define UKE_USB_DWC3_DEV_RESET           1U
#define UKE_USB_DWC3_DEV_CONNECT_DONE    2U
#define UKE_USB_DWC3_DEV_LINK_STATUS     3U
#define UKE_USB_DWC3_DEV_WAKEUP          4U
#define UKE_USB_DWC3_DEV_HIBER_REQ       5U
#define UKE_USB_DWC3_DEV_SUSPEND         6U
#define UKE_USB_DWC3_DEV_SOF             7U
#define UKE_USB_DWC3_DEV_ERRATIC         9U
#define UKE_USB_DWC3_DEV_CMD_CMPL        10U
#define UKE_USB_DWC3_DEV_OVERFLOW        11U

// Endpoint event codes.
#define UKE_USB_DWC3_EP_XFER_COMPLETE    1U
#define UKE_USB_DWC3_EP_XFER_IN_PROGRESS 2U
#define UKE_USB_DWC3_EP_XFER_NOT_READY   3U
#define UKE_USB_DWC3_EP_CMD_CMPL         7U

// TRB control bits.
#define UKE_USB_DWC3_TRB_HWO      (1U << 0)
#define UKE_USB_DWC3_TRB_LST      (1U << 1)
#define UKE_USB_DWC3_TRB_CHN      (1U << 2)
#define UKE_USB_DWC3_TRB_CSP      (1U << 3)
#define UKE_USB_DWC3_TRB_TRBCTL(n) (((n) & 0x3fU) << 4)
#define UKE_USB_DWC3_TRB_ISP_IMI  (1U << 10)
#define UKE_USB_DWC3_TRB_IOC      (1U << 11)
#define UKE_USB_DWC3_TRB_SIZE_LENGTH(n) ((n) & 0xffffffU)
#define UKE_USB_DWC3_TRB_SIZE_STS_MASK  (0xfU << 28)
#define UKE_USB_DWC3_TRB_STS_OK         0U
#define UKE_USB_DWC3_TRB_STS_SETUP_PENDING 2U
#define UKE_USB_DWC3_TRB_STS_XFER_IN_PROG  4U

// TRB control types.
#define UKE_USB_DWC3_TRBCTL_NORMAL         UKE_USB_DWC3_TRB_TRBCTL(1)
#define UKE_USB_DWC3_TRBCTL_CONTROL_SETUP  UKE_USB_DWC3_TRB_TRBCTL(2)
#define UKE_USB_DWC3_TRBCTL_CONTROL_STATUS2 UKE_USB_DWC3_TRB_TRBCTL(3)
#define UKE_USB_DWC3_TRBCTL_CONTROL_STATUS3 UKE_USB_DWC3_TRB_TRBCTL(4)
#define UKE_USB_DWC3_TRBCTL_CONTROL_DATA   UKE_USB_DWC3_TRB_TRBCTL(5)
#define UKE_USB_DWC3_TRBCTL_LINK           UKE_USB_DWC3_TRB_TRBCTL(8)

// Endpoint command types (Linux core.h).
#define UKE_USB_DWC3_DEPCMD_SETEPCONFIG      (0x01U << 0)
#define UKE_USB_DWC3_DEPCMD_SETTRANSFRESOURCE (0x02U << 0)
#define UKE_USB_DWC3_DEPCMD_SETSTALL         (0x04U << 0)
#define UKE_USB_DWC3_DEPCMD_CLEARSTALL       (0x05U << 0)
#define UKE_USB_DWC3_DEPCMD_STARTTRANSFER    (0x06U << 0)
#define UKE_USB_DWC3_DEPCMD_UPDATETRANSFER   (0x07U << 0)
#define UKE_USB_DWC3_DEPCMD_ENDTRANSFER      (0x08U << 0)
#define UKE_USB_DWC3_DEPCMD_DEPSTARTCFG      (0x09U << 0)
#define UKE_USB_DWC3_DEPCMD_CMDACT           (1U << 10)
#define UKE_USB_DWC3_DEPCMD_STATUS(x)        (((x) >> 12) & 0x0fU)
#define UKE_USB_DWC3_DEPCMD_PARAM(x)         ((x) << 16)
#define UKE_USB_DWC3_DEPCMD_GET_RSC_IDX(x)   (((x) >> 16) & 0x7fU)

// DEPCFG parameter fields (Linux gadget.h).
#define UKE_USB_DWC3_DEPCFG_EP_TYPE(n)        (((n) & 0x3U) << 1)
#define UKE_USB_DWC3_DEPCFG_MAX_PACKET_SIZE(n) (((n) & 0x7ffU) << 3)
#define UKE_USB_DWC3_DEPCFG_FIFO_NUMBER(n)    (((n) & 0x1fU) << 17)
#define UKE_USB_DWC3_DEPCFG_BURST_SIZE(n)     (((n) & 0xfU) << 22)
#define UKE_USB_DWC3_DEPCFG_ACTION_INIT       (0U << 30)
#define UKE_USB_DWC3_DEPCFG_ACTION_MODIFY     (2U << 30)
#define UKE_USB_DWC3_DEPCFG_XFER_COMPLETE_EN  (1U << 8)
#define UKE_USB_DWC3_DEPCFG_XFER_IN_PROGRESS_EN (1U << 9)
#define UKE_USB_DWC3_DEPCFG_XFER_NOT_READY_EN (1U << 10)
#define UKE_USB_DWC3_DEPCFG_BINTERVAL_M1(n)   (((n) & 0xffU) << 16)
#define UKE_USB_DWC3_DEPCFG_EP_NUMBER(n)      (((n) & 0x1fU) << 25)

// Endpoint transfer types.
#define UKE_USB_DWC3_EP_TYPE_CONTROL 0U
#define UKE_USB_DWC3_EP_TYPE_ISOC    1U
#define UKE_USB_DWC3_EP_TYPE_BULK    2U
#define UKE_USB_DWC3_EP_TYPE_INTR    3U

#endif /* UKE_USB_DWC3_H */
