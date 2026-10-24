// SPDX-License-Identifier: BSD-2-Clause-Patent
// RPMH rail voting for the eUSB2 PHY (uke/SM7675).
//
// The uke hsphy@88e3000 draws vdd (L2B, 0.88 V) and vdda12 (L4B, 1.2 V) from
// the PMXR2230 PMIC through rpmh regulators marked qcom,proxy-consumer-enable:
// ABL's proxy votes are released at the kernel handoff, which leaves the PHY
// register file alive (wrapper/NOC power) with its digital core unpowered —
// the APB bridge silent, CSFTRST never completing and every endpoint command
// hanging. The stock kernel re-votes through rpmh; this port performs the
// equivalent ACTIVE_ONLY VRM votes directly through apps_rsc drv-2.
//
// Protocol transcribed from the vendor tree (Xiaomi_Kernel_OpenSource-uke):
// drivers/soc/qcom/{cmd-db.c,rpmh-rsc.c}, drivers/regulator/rpmh-regulator.c.
#ifndef UKE_USB_RPMH_H
#define UKE_USB_RPMH_H

#include <stdint.h>

// aop_cmd_db_region@81c60000 (0x20000), DT reserved-memory.
constexpr uint64_t kUkeCmdDbBase = 0x81c60000;
constexpr uint32_t kUkeCmdDbSize = 0x20000;
// apps_rsc@17a00000 drv-2: reg <0x17a20000 0x10000>, qcom,tcs-offset 0xd00,
// channel 0 tcs-config ACTIVE x3 / SLEEP x2 / WAKE x2 / CONTROL x0 / FAST x1.
constexpr uint64_t kUkeRscDrv2Base = 0x17a20000;
constexpr uint32_t kUkeRscDrv2Size = 0x10000;
constexpr uint32_t kUkeRscDrv2TcsOffset = 0xd00;
constexpr unsigned kUkeRscActiveTcsFirst = 0;
constexpr unsigned kUkeRscActiveTcsCount = 3;

// Command DB (little-endian throughout).
constexpr uint8_t kUkeCmdDbMagic[4] = { 0xdb, 0x30, 0x03, 0x0c };
constexpr unsigned kUkeCmdDbMaxSlv = 8;
constexpr uint16_t kUkeCmdDbHwArc = 0x3; // include/soc/qcom/cmd-db.h:
constexpr uint16_t kUkeCmdDbHwVrm = 0x4; // INVALID, MIN=ARC=3, VRM=4, BCM=5.
constexpr unsigned kUkeCmdDbStandaloneBit = 0;

struct UkeCmdDbEntry {
	uint8_t id[8];
	uint32_t priority[2];
	uint32_t addr;
	uint16_t len;
	uint16_t offset;
};

struct UkeCmdDbRscHdr {
	uint16_t slvId;
	uint16_t headerOffset;
	uint16_t dataOffset;
	uint16_t count;
	uint16_t version;
	uint16_t reserved[3];
};

struct UkeCmdDbHeader {
	uint32_t version;
	uint8_t magic[4];
	UkeCmdDbRscHdr header[kUkeCmdDbMaxSlv];
	uint32_t checksum;
	uint32_t reserved;
	uint8_t data[];
};

// Per-DRV register offsets. The table is picked at runtime from RSC_DRV_ID
// (drv base + 0): major >= 3 uses the 3.0 layout. Both generations keep the
// TCS stride at 672 bytes and put the common IRQ registers at tcs base + 0.
struct UkeRscRegs {
	uint32_t tcsStride;
	uint32_t cmdStride;
	uint32_t solverConfig;   // drv base relative
	uint32_t prntChldConfig; // drv base relative
	uint32_t irqEnable;      // tcs base relative, one bit per TCS
	uint32_t irqStatus;
	uint32_t irqClear;
	uint32_t cmdWaitForCmpl; // TCS relative, one bit per command
	uint32_t control;
	uint32_t status;         // nonzero when the TCS is idle
	uint32_t cmdEnable;
	uint32_t cmdMsgid;       // TCS relative, cmdStride per command
	uint32_t cmdAddr;
	uint32_t cmdData;
	uint32_t cmdStatus;
	uint32_t cmdRespData;
};

constexpr UkeRscRegs kUkeRscRegsV27 = { 672, 20, 0x04, 0x0c,
	0x00, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c, 0x40 };
constexpr UkeRscRegs kUkeRscRegsV30 = { 672, 24, 0x04, 0x0c,
	0x00, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c, 0x40, 0x44 };

constexpr uint32_t kUkeRscDrvId = 0x000; // major [23:16], minor [15:8]
constexpr uint32_t kUkeTcsAmcEnable = 1u << 16;
constexpr uint32_t kUkeTcsAmcTrigger = 1u << 24;
constexpr uint32_t kUkeCmdMsgidLen = 8;
constexpr uint32_t kUkeCmdMsgidRespReq = 1u << 8;
constexpr uint32_t kUkeCmdMsgidWrite = 1u << 16;
constexpr unsigned kUkeDrvNumTcsShift = 6;
constexpr uint32_t kUkeDrvNumTcsMask = 0x3f;
constexpr unsigned kUkeDrvNcptShift = 27;
constexpr uint32_t kUkeDrvNcptMask = 0x1f;

// VRM voting registers are cmd-db addr + 4 * index (rpmh-regulator.c):
// VOLTAGE=0 (data in millivolts), ENABLE=1 (data 1), MODE=2 (HPM = 4 per the
// uke DT qcom,init-mode). An enable command must be the last of the message.
struct UkeVrmRail {
	const char *resource; // cmd-db id, DT qcom,resource-name
	uint32_t millivolts;
	uint32_t mode;
};

constexpr UkeVrmRail kUkeUsbPhyRails[] = {
	{ "ldob2", 880, 4 }, // pmxr2230_l2: vdd, 0.88 V
	{ "ldob4", 1200, 4 }, // pmxr2230_l4: vdda12, 1.2 V
};
constexpr unsigned kUkeUsbPhyRailCount = sizeof(kUkeUsbPhyRails) / sizeof(kUkeUsbPhyRails[0]);

#endif // UKE_USB_RPMH_H
