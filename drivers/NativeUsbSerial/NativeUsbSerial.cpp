// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeUsbSerial: uke (SM7675) USB2 peripheral debug channel.
//
// native-usb-probe=1 is read-only: it maps the USB wrapper, QSCRATCH, eUSB2
// PHY, GCC/GDSC, TCSR and apps-SMMU windows and prints what ABL/UEFI left
// behind. native-usb-serial=1 performs the warm handoff, enumerates a
// CDC-ACM device and relays the kernel msgbuf as bulk IN data.
//
// The controller path is polled: a 1 ms thread services the DWC3 event ring,
// EP0 control transfers and the two bulk endpoints; no GIC interrupt is
// registered. This keeps the driver independent of the platform interrupt
// plumbing and is sufficient for a debug log.
//
// Board facts are in UkeUsbBoard.h/UkeUsbGcc.h/UkeUsbPhy.h/UkeUsbDwc3.h.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <sys/msgbuf.h>

#include "UkeUsbBoard.h"
#include "UkeUsbGcc.h"
#include "UkeUsbDwc3.h"
#include "UkeUsbLog.h"
#include "UkeUsbPhy.h"
#include "UkeUsbRpmh.h"
#include "UsbDescriptors.h"
#include "UsbRing.h"
#include "SmmuBypass.h"

extern "C" struct msgbuf *msgbufp;
extern "C" void bsd_log_lock_safe(void);
extern "C" void bsd_log_unlock(void);

namespace {

uintptr_t
mapPhys(uint64_t physical, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
	IODeviceMemory *memory = IODeviceMemory::withRange(physical, length);
	if (memory == nullptr) {
		return 0;
	}
	keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
	memory->release();
	return keep ? static_cast<uintptr_t>(keep->getVirtualAddress()) : 0;
}

inline uint32_t
rd32(uintptr_t base, uint32_t offset)
{
	return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

inline void
wr32(uintptr_t base, uint32_t offset, uint32_t value)
{
	*reinterpret_cast<volatile uint32_t *>(base + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

// SMCCC SMC invocation (x0-x3 carry the arguments and results; the callee
// may clobber x4-x17). Used for the SCM SIO service, which performs the
// MMIO from EL3 where the CPU-side store to the TCSR region only stalls.
uint64_t
scmSmc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t *out1)
{
	register uint64_t x0 __asm__("x0") = a0;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
	    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
	    :
	    : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
	      "x14", "x15", "x16", "x17", "memory");
	if (out1 != nullptr) {
		*out1 = x1;
	}
	return x0;
}

void
cacheCivac(const void *start, size_t length)
{
	uintptr_t address = reinterpret_cast<uintptr_t>(start) & ~uintptr_t(63);
	uintptr_t end = reinterpret_cast<uintptr_t>(start) + length;
	for (; address < end; address += 64) {
		__asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

void
cacheInvalidate(const void *start, size_t length)
{
	__asm__ volatile("dsb sy" ::: "memory");
	uintptr_t address = reinterpret_cast<uintptr_t>(start);
	for (uintptr_t end = address + length; address < end; address += 64) {
		__asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

inline uint16_t
le16(const uint8_t *p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

bool
compatibleHas(const OSData *data, const char *wanted)
{
	const char *bytes = static_cast<const char *>(data->getBytesNoCopy());
	const size_t length = data->getLength();
	const size_t wantedLength = strlen(wanted);
	if (bytes == nullptr || wantedLength == 0 || length < wantedLength) {
		return false;
	}
	for (size_t at = 0; at + wantedLength <= length; at++) {
		if (memcmp(bytes + at, wanted, wantedLength) != 0) {
			continue;
		}
		if (at + wantedLength == length || bytes[at + wantedLength] == '\0') {
			return true;
		}
	}
	return false;
}

} // namespace

class NativeUsbSerial : public IOService {
	OSDeclareDefaultStructors(NativeUsbSerial)

	// Probe mappings.
	OSSharedPtr<IOMemoryMap> fWrapperMap;
	OSSharedPtr<IOMemoryMap> fQscratchMap;
	OSSharedPtr<IOMemoryMap> fPhyMap;
	OSSharedPtr<IOMemoryMap> fEudEnableMap;
	OSSharedPtr<IOMemoryMap> fEudDetectMap;
	OSSharedPtr<IOMemoryMap> fGccMap;
	OSSharedPtr<IOMemoryMap> fPhyBcrMap;
	OSSharedPtr<IOMemoryMap> fTcsrMap;
	OSSharedPtr<IOMemoryMap> fSmmuMap;
	OSSharedPtr<IOMemoryMap> fCmdDbMap;
	OSSharedPtr<IOMemoryMap> fRscMap;

	uintptr_t fWrapper = 0;
	uintptr_t fQscratch = 0;
	uintptr_t fPhy = 0;
	uintptr_t fEudEnable = 0;
	uintptr_t fEudDetect = 0;
	uintptr_t fGcc = 0;
	uintptr_t fPhyBcr = 0;
	uintptr_t fTcsr = 0;
	uintptr_t fSmmu = 0;
	uintptr_t fCmdDb = 0;
	uintptr_t fRsc = 0;

	// DMA layout. One physically contiguous, 4 KiB-aligned allocation.
	enum {
		kDmaSize = 0x10000,
		kEventOffset = 0x0000,
		kEp0OutTrbOffset = 0x1000,
		kEp0InTrbOffset = 0x2000,
		kEp1InTrbOffset = 0x3000,
		kEp2InTrbOffset = 0x4000,
		kEp3OutTrbOffset = 0x5000,
		kSetupOffset = 0x6000,
		kStatusOffset = 0x7000,
		kEp0DataOffset = 0x8000,
		kEp2DataOffset = 0xa000,
		kEp3DataOffset = 0xb000,
		kTrbEntries = 256,
		kEventEntries = 256,
		kTxBytes = 0x4000,
		kMaxTransfer = 512,
		kLogChunk = 4096,
	};

	struct Endpoint {
		uint8_t phys = 0;
		UkeUsbTrb *trbs = nullptr;
		uint64_t trbsPhys = 0;
		uint32_t resource = 0;
		uint32_t bufferSize = 0;
		bool enabled = false;
		bool busy = false;
	};

	IOBufferMemoryDescriptor *fDma = nullptr;
	uint8_t *fDmaVirt = nullptr;
	uint64_t fDmaPhys = 0;
	uint32_t fDmaSize = 0;

	UkeUsbEvent *fEvents = nullptr;
	uint64_t fEventsPhys = 0;
	uint32_t fEventHead = 0;

	Endpoint fEp0Out;
	Endpoint fEp0In;
	Endpoint fEp1In;
	Endpoint fEp2In;
	Endpoint fEp3Out;

	uint8_t *fSetup = nullptr;
	uint64_t fSetupPhys = 0;
	uint8_t *fStatus = nullptr;
	uint64_t fStatusPhys = 0;
	uint8_t *fEp0Data = nullptr;
	uint64_t fEp0DataPhys = 0;
	uint8_t *fEp2Data = nullptr;
	uint64_t fEp2DataPhys = 0;
	uint8_t *fEp3Data = nullptr;
	uint64_t fEp3DataPhys = 0;

	UkeUsbDescriptors fDescriptors;
	uint8_t fLineCoding[7] = {};
	bool fConfigured = false;
	bool fSerialReady = false;
	bool fEp0Waiting = false;
	volatile bool fStop = false;
	bool fSerialStarted = false;
	volatile bool fLogPrimed = false;
	volatile bool fDump = false;
	bool fEnablePhy = false;
	bool fEnableTcsr = false;
	bool fEnableScm = false;
	bool fEnableRails = false;
	bool fEnableQscratch = false;
	bool fSoftReset = true;
	bool fBlockReset = false;
	bool fForceFs = false;
	uint32_t fProbeLevel = 0;
	uint64_t fQscratchBase = UKE_USB_QSCRATCH_BASE;
	UkeRscRegs fRscRegs = kUkeRscRegsV30;

	uint8_t *fTxMem = nullptr;
	UkeUsbByteRing fTxRing;
	IOLock *fLock = nullptr;
	UkeUsbLogCursor fLogCursor;

	uint8_t fCmd[160] = {};
	uint32_t fCmdLength = 0;
	uint32_t fSetupLog = 0;
	uint32_t fEventLog = 0;

	// Probe helpers.
	void releaseMaps(void);
	bool mapWindows(void);
	void dumpProbe(void);
	void dumpSmmu(void);
	void dumpPhy(void);

	// Serial helpers.
	bool serialStart(void);
	void serialStop(void);
	bool warmInit(void);
	bool blockReset(void);
	void phyApbEchoTest(void);
	void phyApbWrite(uint8_t reg, uint8_t value);
	void phyApbRead(uint8_t reg, uint8_t expected);
	void scmClkrefProbe(void);
	bool railsVote(void);
	bool tcsVoteActive(unsigned tcs, const uint32_t *addrs, const uint32_t *datas,
	    unsigned count, const char *name);
	bool dmaAllocate(void);
	bool smmuAttach(void);
	void fifoConfig(void);
	void deviceInit(void);
	void eventInit(void);
	bool configEp(Endpoint &ep, uint8_t type, uint16_t mps, uint32_t fifo, bool in, bool ep0);
	bool depCommand(uint8_t phys, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t *result);
	bool startTransfer(Endpoint &ep);
	bool waitTrb(UkeUsbTrb *trb, unsigned timeoutMs);
	void ep0Arm(void);
	void ep0Poll(void);
	void handleSetup(const uint8_t *setup);
	void ep0DataIn(const uint8_t *data, uint32_t length);
	bool ep0DataOut(uint32_t length);
	void ep0Status(bool in);
	void ep0Stall(void);
	bool configureData(void);
	void serviceEvents(void);
	void serviceTx(void);
	void serviceRx(void);
	void processCommand(void);
	void queueBanner(const char *text);
	void pumpMsgbuf(void);
	void serialLoop(void);
	void logLoop(void);

	static uint32_t smmuRead(void *context, unsigned offset);
	static void smmuWrite(void *context, unsigned offset, uint32_t value);
	static void usbThreadEntry(void *context, wait_result_t result);
	static void logThreadEntry(void *context, wait_result_t result);

public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	void free(void) override;
};

OSDefineMetaClassAndStructors(NativeUsbSerial, IOService)

void
NativeUsbSerial::releaseMaps(void)
{
	fWrapperMap.reset();
	fQscratchMap.reset();
	fPhyMap.reset();
	fEudEnableMap.reset();
	fEudDetectMap.reset();
	fGccMap.reset();
	fPhyBcrMap.reset();
	fTcsrMap.reset();
	fSmmuMap.reset();
	fCmdDbMap.reset();
	fRscMap.reset();
	fWrapper = fQscratch = fPhy = fEudEnable = fEudDetect = 0;
	fGcc = fPhyBcr = fTcsr = fSmmu = fCmdDb = fRsc = 0;
}

bool
NativeUsbSerial::mapWindows(void)
{
	fWrapper = mapPhys(UKE_USB_WRAPPER_BASE, UKE_USB_WRAPPER_SIZE, fWrapperMap);
	fQscratch = mapPhys(fQscratchBase, UKE_USB_QSCRATCH_SIZE, fQscratchMap);
	fPhy = mapPhys(UKE_USB_PHY_BASE, UKE_USB_PHY_SIZE, fPhyMap);
	fEudEnable = mapPhys(UKE_USB_EUD_ENABLE_BASE, 0x10, fEudEnableMap);
	fEudDetect = mapPhys(UKE_USB_EUD_DETECT_BASE, 0x10, fEudDetectMap);
	fGcc = mapPhys(UKE_USB_GDSC_BASE, UKE_USB_GDSC_SIZE, fGccMap);
	fPhyBcr = mapPhys(UKE_USB_GCC_BASE + 0x12000ULL, 0x1000, fPhyBcrMap);
	fTcsr = mapPhys(UKE_USB_TCSR_USB2_CLKREF_PAGE, 0x1000, fTcsrMap);
	fSmmu = mapPhys(UKE_USB_SMMU_BASE, UKE_USB_SMMU_SIZE, fSmmuMap);

	if (!fWrapper || !fQscratch || !fPhy || !fEudEnable || !fEudDetect ||
	    !fGcc || !fPhyBcr || !fTcsr || !fSmmu) {
		IOLog("NativeUsbSerial: map failed wrapper=%p qscratch=%p phy=%p eud=%p/%p gcc=%p phybcr=%p tcsr=%p smmu=%p\n",
		    reinterpret_cast<void *>(fWrapper), reinterpret_cast<void *>(fQscratch),
		    reinterpret_cast<void *>(fPhy), reinterpret_cast<void *>(fEudEnable),
		    reinterpret_cast<void *>(fEudDetect), reinterpret_cast<void *>(fGcc),
		    reinterpret_cast<void *>(fPhyBcr), reinterpret_cast<void *>(fTcsr),
		    reinterpret_cast<void *>(fSmmu));
		return false;
	}
	IOLog("NativeUsbSerial: maps wrapper=%llx qscratch=%llx phy=%llx gcc=%llx phybcr=%llx tcsr=%llx smmu=%llx eud=%llx/%llx\n",
	    static_cast<unsigned long long>(fWrapper), static_cast<unsigned long long>(fQscratch),
	    static_cast<unsigned long long>(fPhy), static_cast<unsigned long long>(fGcc),
	    static_cast<unsigned long long>(fPhyBcr), static_cast<unsigned long long>(fTcsr),
	    static_cast<unsigned long long>(fSmmu), static_cast<unsigned long long>(fEudEnable),
	    static_cast<unsigned long long>(fEudDetect));
	return true;
}

void
NativeUsbSerial::dumpPhy(void)
{
	static const uint16_t offsets[] = {
		0x000, 0x03c, 0x050, 0x054, 0x058, 0x05c,
		0x060, 0x064, 0x068, 0x06c, 0x070, 0x078,
		0x07c, 0x094, 0x098, 0x0b8, 0x130, 0x134,
		0x138, 0x13c, 0x140, 0x144, 0x148,
	};
	unsigned index = 0;
	for (uint16_t offset : offsets) {
		IOLog("NativeUsbSerial: PHY[%03x]=%08x%s",
		    offset, rd32(fPhy, offset),
		    ((index & 3U) == 3U) ? "\n" : " ");
		index++;
	}
	if ((index & 3U) != 0) {
		IOLog("\n");
	}
	IOLog("NativeUsbSerial: PHY init-table entries=%u eud-enable=%08x eud-detect=%08x\n",
	    static_cast<unsigned>(UKE_USB_PHY_INIT_COUNT),
	    rd32(fEudEnable, 0), rd32(fEudDetect, 0));
	IOLog("NativeUsbSerial: PHY 38.4MHz table: ctrl2=%02x ctrl3=%02x override=%02x\n",
	    kUkeUsbPhyInit38_4Mhz[7].value, kUkeUsbPhyInit38_4Mhz[8].value,
	    kUkeUsbPhyInit38_4Mhz[16].value);
}

void
NativeUsbSerial::dumpSmmu(void)
{
	const uint32_t idr0 = rd32(fSmmu, 0x20);
	const uint32_t idr1 = rd32(fSmmu, 0x24);
	const uint32_t idr2 = rd32(fSmmu, 0x28);
	const unsigned streams = idr0 & 0xffU;
	const unsigned banks = idr1 & 0xffU;
	const unsigned pages = 1U << (((idr1 >> 28) & 7U) + 1U);
	unsigned valid = 0;
	unsigned usbSlot = ~0U;
	IOLog("NativeUsbSerial: SMMU idr0=%08x idr1=%08x idr2=%08x scr0=%08x gfsr=%08x streams=%u banks=%u pages=%u\n",
	    idr0, idr1, idr2, rd32(fSmmu, 0x00), rd32(fSmmu, 0x48),
	    streams > 128 ? 128U : streams, banks, pages);
	for (unsigned slot = 0; slot < streams && slot < 128; slot++) {
		const uint32_t smr = rd32(fSmmu, 0x800 + 4 * slot);
		if ((smr & 0x80000000U) == 0) {
			continue;
		}
		const uint32_t s2cr = rd32(fSmmu, 0xc00 + 4 * slot);
		const uint32_t stream = smr & 0xffffU;
		valid++;
		if (stream == UKE_USB_STREAM_ID) {
			usbSlot = slot;
		}
		IOLog("NativeUsbSerial: SMMU slot=%u smr=%08x s2cr=%08x stream=%02x%s\n",
		    slot, smr, s2cr, stream, stream == UKE_USB_STREAM_ID ? " (usb)" : "");
	}
	IOLog("NativeUsbSerial: SMMU valid-streams=%u usb-slot=%d\n",
	    valid, usbSlot == ~0U ? -1 : static_cast<int>(usbSlot));
	if (banks != 0 && pages <= 128 && (pages + banks) * 4096U <= UKE_USB_SMMU_SIZE) {
		const unsigned bank = banks - 1;
		const unsigned cb = (pages + bank) * 4096U;
		IOLog("NativeUsbSerial: SMMU top-bank=%u sctlr=%08x cbar=%08x cba2r=%08x\n",
		    bank, rd32(fSmmu, cb), rd32(fSmmu, 0x1000 + 4 * bank),
		    rd32(fSmmu, 0x1800 + 4 * bank));
	}
}

void
NativeUsbSerial::dumpProbe(void)
{
	IOLog("NativeUsbSerial: probing uke USB2 wrapper base=%08x level=%u phy=%u tcsr=%u bcr=%u\n",
	    static_cast<unsigned>(UKE_USB_WRAPPER_BASE), fProbeLevel, fEnablePhy ? 1U : 0U,
	    fEnableTcsr ? 1U : 0U, fBlockReset ? 1U : 0U);
	IOLog("NativeUsbSerial: GCC gdscr=%08x cfg=%08x master=%08x sleep=%08x utmi=%08x cfg_noc=%08x aggre=%08x\n",
	    rd32(fGcc, 0x004), rd32(fGcc, 0x008), rd32(fGcc, 0x018),
	    rd32(fGcc, 0x024), rd32(fGcc, 0x028), rd32(fGcc, 0x088),
	    rd32(fGcc, 0x08c));

	// GCC-only reads. The TCSR page (0x1fb1000) is write-only to us: the
	// old 0x1f4b000 page was the ADSP ipc_hwmutex region and its read hung
	// the tablet; the real clkref page is ordinary TCSR land (the stock
	// kernel reads and writes it freely) but there is no need to risk it.
	const uint32_t usb30Bcr = rd32(fGcc, 0x000);
	IOLog("NativeUsbSerial: GCC usb30-bcr=%08x\n", usb30Bcr);
	const uint32_t phyBcr = rd32(fPhyBcr, 0x000);
	IOLog("NativeUsbSerial: GCC qusb2phy-bcr=%08x\n", phyBcr);

	if (fEnableTcsr) {
		IOLog("NativeUsbSerial: probe: TCSR clkref at %llx (write-only)\n",
		    static_cast<unsigned long long>(UKE_USB_TCSR_USB2_CLKREF_EN));
	}

	if (fProbeLevel >= 2) {
		if ((usb30Bcr & UKE_USB_GCC_RESET_ASSERT) != 0) {
			IOLog("NativeUsbSerial: DWC3 reset asserted; skipping DWC3/QSCRATCH reads\n");
		} else {
			IOLog("NativeUsbSerial: DWC3 gsnpsid=%08x guid=%08x gctl=%08x gsts=%08x\n",
			    rd32(fWrapper, UKE_USB_DWC3_GSNPSID), rd32(fWrapper, UKE_USB_DWC3_GUID),
			    rd32(fWrapper, UKE_USB_DWC3_GCTL), rd32(fWrapper, UKE_USB_DWC3_GSTS));
			IOLog("NativeUsbSerial: DWC3 hwparams=%08x %08x %08x %08x\n",
			    rd32(fWrapper, UKE_USB_DWC3_GHWPARAMS0), rd32(fWrapper, UKE_USB_DWC3_GHWPARAMS1),
			    rd32(fWrapper, UKE_USB_DWC3_GHWPARAMS2), rd32(fWrapper, UKE_USB_DWC3_GHWPARAMS3));
			IOLog("NativeUsbSerial: DWC3 gusb2phycfg=%08x gusb3pipectl=%08x dcfg=%08x dctl=%08x devten=%08x dsts=%08x\n",
			    rd32(fWrapper, UKE_USB_DWC3_GUSB2PHYCFG(0)),
			    rd32(fWrapper, UKE_USB_DWC3_GUSB3PIPECTL(0)),
			    rd32(fWrapper, UKE_USB_DWC3_DCFG), rd32(fWrapper, UKE_USB_DWC3_DCTL),
			    rd32(fWrapper, UKE_USB_DWC3_DEVTEN), rd32(fWrapper, UKE_USB_DWC3_DSTS));
			IOLog("NativeUsbSerial: DWC3 event addr=%08x:%08x size=%08x count=%08x\n",
			    rd32(fWrapper, UKE_USB_DWC3_GEVNTADRHI), rd32(fWrapper, UKE_USB_DWC3_GEVNTADRLO),
			    rd32(fWrapper, UKE_USB_DWC3_GEVNTSIZ), rd32(fWrapper, UKE_USB_DWC3_GEVNTCOUNT));
			if (fEnableQscratch) {
				IOLog("NativeUsbSerial: QSCRATCH general=%08x hsphy=%08x\n",
				    rd32(fQscratch, 0x08), rd32(fQscratch, 0x10));
			} else {
				IOLog("NativeUsbSerial: QSCRATCH skipped (native-usb-qscratch not set)\n");
			}
		}
	}

	if (fEnablePhy) {
		IOLog("NativeUsbSerial: probe: reading PHY\n");
		dumpPhy();
	}
	dumpSmmu();
}

uint32_t
NativeUsbSerial::smmuRead(void *context, unsigned offset)
{
	NativeUsbSerial *self = static_cast<NativeUsbSerial *>(context);
	return rd32(self->fSmmu, offset);
}

void
NativeUsbSerial::smmuWrite(void *context, unsigned offset, uint32_t value)
{
	NativeUsbSerial *self = static_cast<NativeUsbSerial *>(context);
	wr32(self->fSmmu, offset, value);
}

bool
NativeUsbSerial::warmInit(void)
{
	// Reference sequence, from the K70 CDC driver this port follows
	// (xnu/vermeer/vermeer_usb_debug.c): verify that the bootloader left the
	// USB block powered and clocked, then touch nothing but the DWC3 core.
	// No GCC writes, no PHY programming, no TCSR: that is ABL state, and
	// rewriting it is what made the earlier revisions fail.
	uint32_t gdscr = rd32(fGcc, 0x004);
	uint32_t cfg = rd32(fGcc, 0x008);
	uint32_t master = rd32(fGcc, 0x018);
	uint32_t sleepClk = rd32(fGcc, 0x024);
	uint32_t utmi = rd32(fGcc, 0x028);
	uint32_t cfgNoc = rd32(fGcc, 0x088);
	uint32_t aggre = rd32(fGcc, 0x08c);
	IOLog("NativeUsbSerial: warm: gcc gdscr=%08x cfg=%08x master=%08x sleep=%08x utmi=%08x cfg_noc=%08x aggre=%08x\n",
	    gdscr, cfg, master, sleepClk, utmi, cfgNoc, aggre);
	if ((gdscr & (1U << 31)) == 0) {
		IOLog("NativeUsbSerial: USB block not powered by the bootloader; controller untouched\n");
		return false;
	}
	if (((master | cfgNoc | aggre) & (1U << 31)) != 0) {
		IOLog("NativeUsbSerial: USB clocks not enabled by the bootloader; controller untouched\n");
		return false;
	}

	// Pure-warm APB echo test BEFORE touching anything: every bridge probe
	// so far ran after the PHY BCR pulse and the reprogramming table, so
	// "the bridge is dead" was never measured on the untouched ABL state.
	// An echo here means the firmware-held reference clock is on and OUR
	// pulse+table breaks the PHY digital; silence means the reference never
	// arrived in the first place.
	IOLog("NativeUsbSerial: warm: apb echo (pure warm)\n");
	phyApbEchoTest();

	// The USB2 reference-clock gate (tcsrcc USB2_CLKREF_EN at 0x1fb1118).
	// Opt-in only: the store at that address stalls the whole machine
	// (usbbr5) and every other TCSR access stalls too (usbbr7), so the
	// reference clock cannot be switched from here at all.
	if (fEnableTcsr) {
		IOLog("NativeUsbSerial: warm: tcsr clkref enable (0x1fb1118)\n");
		wr32(fTcsr, UKE_USB_TCSR_USB2_CLKREF_OFF, 1);
		IODelay(100);
	}
	if (fEnablePhy) {
		IOLog("NativeUsbSerial: warm: phy (opt-in)\n");
		wr32(fPhyBcr, 0x000, 1);
		IODelay(100);
		wr32(fPhyBcr, 0x000, 0);
		IODelay(100);
		// The BCR pulse resets the glue's clock routing too: ABL left
		// GENERAL_CFG with UTMI-as-pipe and the PIPE3 status override, and
		// without those bits the core's pipe-clock input has no source and
		// the endpoint command engine never runs (usbdal5's DEP0 timeout).
		uint32_t general = rd32(fQscratch, UKE_USB_QSCRATCH_GENERAL_CFG);
		wr32(fQscratch, UKE_USB_QSCRATCH_GENERAL_CFG,
		    (general & ~UKE_USB_QSCRATCH_PIPE_UTMI_CLK_DIS) |
		    UKE_USB_QSCRATCH_PIPE_UTMI_CLK_SEL |
		    UKE_USB_QSCRATCH_PIPE3_PHYSTATUS_SW);
		IOLog("NativeUsbSerial: warm: general=%08x -> %08x\n",
		    general, rd32(fQscratch, UKE_USB_QSCRATCH_GENERAL_CFG));
		for (size_t i = 0; i < UKE_USB_PHY_INIT_COUNT; i++) {
			const UkeUsbPhyWrite &write = kUkeUsbPhyInit38_4Mhz[i];
			uint32_t before = rd32(fPhy, write.offset);
			uint32_t value = (before & ~write.mask) | (write.value & write.mask);
			wr32(fPhy, write.offset, value);
			uint32_t after = rd32(fPhy, write.offset);
			if ((after & write.mask) != (write.value & write.mask)) {
				IOLog("NativeUsbSerial: PHY[%03x] expected %08x got %08x\n",
				    write.offset, write.value & write.mask, after & write.mask);
				return false;
			}
		}
		// Every wrapper write held, but that only proves the PHY's config
		// register file is mapped. Re-run the echo test after the pulse and
		// the table: if the pure-warm echo passed and this one fails, our
		// reset+reprogramming is what kills the PHY's clock domain.
		IOLog("NativeUsbSerial: warm: apb echo (after pulse+table)\n");
		phyApbEchoTest();
		IOLog("NativeUsbSerial: warm: phy state common0=%08x ctrl2=%08x ctrl3=%08x cfg0=%08x fsel=%08x utmi0=%08x eud=%08x\n",
		    rd32(fPhy, 0x054), rd32(fPhy, 0x05c), rd32(fPhy, 0x060),
		    rd32(fPhy, 0x094), rd32(fPhy, 0x0b8), rd32(fPhy, 0x03c),
		    rd32(fEudEnable, 0));
		IOLog("NativeUsbSerial: warm: hwparams0=%08x 3=%08x\n",
		    rd32(fWrapper, UKE_USB_DWC3_GHWPARAMS0),
		    rd32(fWrapper, UKE_USB_DWC3_GHWPARAMS3));
		if (fEnableScm) {
			scmClkrefProbe();
		}
	}

	// Core identity and port mode. The reference refuses cores other than
	// DWC_usb3 (GSNPSID 5533) and DWC_usb31 (3331) and refuses a core whose
	// GCTL.PrtCapDir is not device mode; log both here instead. ABL should
	// leave device mode behind, but a host-mode core would make every
	// device-domain write below a no-op.
	uint32_t id = rd32(fWrapper, UKE_USB_DWC3_GSNPSID);
	uint32_t gctl = rd32(fWrapper, UKE_USB_DWC3_GCTL);
	unsigned prtcap = (gctl & UKE_USB_DWC3_GCTL_PRTCAP_MASK) >> 12;
	IOLog("NativeUsbSerial: warm: id=%08x gctl=%08x prtcap=%u\n", id, gctl, prtcap);
	if (prtcap != 2) {
		IOLog("NativeUsbSerial: warm: switching prtcap %u -> device\n", prtcap);
		wr32(fWrapper, UKE_USB_DWC3_GCTL,
		    (gctl & ~UKE_USB_DWC3_GCTL_PRTCAP_MASK) | UKE_USB_DWC3_GCTL_PRTCAP_DEVICE);
		IODelay(100);
		IOLog("NativeUsbSerial: warm: gctl now=%08x\n", rd32(fWrapper, UKE_USB_DWC3_GCTL));
	}

	// The stock uke DT asks for snps,disable-clk-gating: with the core
	// halted and gating enabled, the first endpoint command after the halt
	// completed but the next one hung with CmdAct stuck (usbdal4b/5), which
	// is what the core's own clock gating does to an idle device domain.
	gctl = rd32(fWrapper, UKE_USB_DWC3_GCTL);
	if ((gctl & UKE_USB_DWC3_GCTL_DSBLCLKGTNG) == 0) {
		wr32(fWrapper, UKE_USB_DWC3_GCTL, gctl | UKE_USB_DWC3_GCTL_DSBLCLKGTNG);
		IOLog("NativeUsbSerial: warm: gctl %08x -> %08x (disable clk gating)\n",
		    gctl, rd32(fWrapper, UKE_USB_DWC3_GCTL));
	}

	// Stop whatever ABL left running: the core reset below only self-clears
	// once the controller is halted.
	IOLog("NativeUsbSerial: warm: stop dctl=%08x dsts=%08x\n",
	    rd32(fWrapper, UKE_USB_DWC3_DCTL), rd32(fWrapper, UKE_USB_DWC3_DSTS));
	wr32(fWrapper, UKE_USB_DWC3_DEVTEN, 0);
	wr32(fWrapper, UKE_USB_DWC3_DCTL,
	    rd32(fWrapper, UKE_USB_DWC3_DCTL) & ~UKE_USB_DWC3_DCTL_RUN_STOP);
	bool halted = false;
	for (unsigned i = 0; i < 500; i++) {
		if ((rd32(fWrapper, UKE_USB_DWC3_DSTS) & UKE_USB_DWC3_DSTS_DEVCTRLHLT) != 0) {
			halted = true;
			break;
		}
		IOSleep(1);
	}
	if (!halted) {
		IOLog("NativeUsbSerial: controller halt timeout dsts=%08x\n",
		    rd32(fWrapper, UKE_USB_DWC3_DSTS));
		return false;
	}
	if (fBlockReset) {
		if (!blockReset()) {
			return false;
		}
	} else if (fSoftReset) {
		// The device domain's functional logic (event ring, endpoint
		// command engine, the reset sequencer itself) needs its clocks;
		// a stuck CSFTRST with every register downstream reading back 0
		// means those clocks never ran during the reset. ABL leaves the
		// PHY suspend bits set after fastboot, and an idle core may gate
		// its clocks, so clear both classes before asking for a reset.
		uint32_t usb2 = rd32(fWrapper, UKE_USB_DWC3_GUSB2PHYCFG(0));
		uint32_t usb3 = rd32(fWrapper, UKE_USB_DWC3_GUSB3PIPECTL(0));
		IOLog("NativeUsbSerial: warm: pre-reset id=%08x gctl=%08x general=%08x usb2=%08x usb3=%08x\n",
		    id, gctl, rd32(fQscratch, 0x08), usb2, usb3);
		if ((usb2 & UKE_USB_DWC3_GUSB2PHYCFG_SUSPHY) != 0) {
			IOLog("NativeUsbSerial: warm: clearing usb2 suspend\n");
			wr32(fWrapper, UKE_USB_DWC3_GUSB2PHYCFG(0),
			    usb2 & ~UKE_USB_DWC3_GUSB2PHYCFG_SUSPHY);
		}
		if ((usb3 & UKE_USB_DWC3_GUSB3PIPECTL_SUSPHY) != 0) {
			IOLog("NativeUsbSerial: warm: clearing usb3 suspend\n");
			wr32(fWrapper, UKE_USB_DWC3_GUSB3PIPECTL(0),
			    usb3 & ~UKE_USB_DWC3_GUSB3PIPECTL_SUSPHY);
		}
		// Variant 1: keep the clocks ungated while the reset runs.
		wr32(fWrapper, UKE_USB_DWC3_GCTL,
		    rd32(fWrapper, UKE_USB_DWC3_GCTL) | UKE_USB_DWC3_GCTL_DSBLCLKGTNG);

		bool resetDone = false;
		for (unsigned attempt = 0; attempt < 2 && !resetDone; attempt++) {
			if (attempt == 1) {
				// Variant 2: put the glue retiming in place before the
				// reset instead of after it; on a USB2-only board the
				// UTMI-as-pipe configuration may need to be live for
				// the reset to see its clocks.
				uint32_t hs = rd32(fQscratch, 0x10);
				uint32_t ss = rd32(fQscratch, 0x30);
				wr32(fQscratch, 0x10, hs | (1U << 20) | (1U << 28));
				wr32(fQscratch, 0x30, ss | (1U << 24));
				IOLog("NativeUsbSerial: warm: qscratch pre-reset hs=%08x->%08x ss=%08x->%08x\n",
				    hs, rd32(fQscratch, 0x10), ss, rd32(fQscratch, 0x30));
				// A stuck bit holds the domain down; drop it before
				// asking for a reset again.
				wr32(fWrapper, UKE_USB_DWC3_DCTL,
				    rd32(fWrapper, UKE_USB_DWC3_DCTL) & ~UKE_USB_DWC3_DCTL_CSFTRST);
				IODelay(100);
			}
			IODelay(100);
			// The reset bit has to be written alone; carrying DCTL's other
			// bits over is what left CSFTRST stuck in the earlier revisions.
			wr32(fWrapper, UKE_USB_DWC3_DCTL, UKE_USB_DWC3_DCTL_CSFTRST);
			for (unsigned i = 0; i < 500; i++) {
				if ((rd32(fWrapper, UKE_USB_DWC3_DCTL) & UKE_USB_DWC3_DCTL_CSFTRST) == 0) {
					resetDone = true;
					break;
				}
				IOSleep(1);
			}
			IOLog("NativeUsbSerial: warm: reset attempt %u dctl=%08x\n",
			    attempt, rd32(fWrapper, UKE_USB_DWC3_DCTL));
		}
		if (!resetDone) {
			// Software-clear the bit and continue: a permanently stuck bit
			// freezes the whole device domain, and continuing lets the rest
			// of bring-up report what still works instead of stopping here.
			wr32(fWrapper, UKE_USB_DWC3_DCTL,
			    rd32(fWrapper, UKE_USB_DWC3_DCTL) & ~UKE_USB_DWC3_DCTL_CSFTRST);
			IOLog("NativeUsbSerial: DWC3 soft reset did not complete (dctl=%08x usb2=%08x usb3=%08x); cleared manually, continuing\n",
			    rd32(fWrapper, UKE_USB_DWC3_DCTL),
			    rd32(fWrapper, UKE_USB_DWC3_GUSB2PHYCFG(0)),
			    rd32(fWrapper, UKE_USB_DWC3_GUSB3PIPECTL(0)));
		} else {
			IOLog("NativeUsbSerial: warm: dwc3-reset done dctl=%08x\n",
			    rd32(fWrapper, UKE_USB_DWC3_DCTL));
		}
	} else {
		IOLog("NativeUsbSerial: warm: dwc3-reset skipped\n");
	}

	IOLog("NativeUsbSerial: warm: done\n");
	return true;
}

// Full block reset through GCC_USB30_PRIM_BCR, mirroring the vendor glue's
// dwc3_msm_link_clk_reset(): the branch clocks go down first, the reset
// pulses, then the clocks come back in the glue's order (iface before core,
// which has an FSM dependency). This is the only path that clears whatever
// endpoint state ABL's fastboot left behind — CSFTRST never self-clears here
// and SETEPCONFIG hangs on top of the warm state even after DALEPENA is
// dropped. The core comes back empty, so the glue routing, port mode and the
// two PHY control registers are all replayed from the captured ABL values.
bool
NativeUsbSerial::blockReset(void)
{
	static const uint32_t kDown[] = { 0x028, 0x024, 0x018, 0x008 };
	static const uint32_t kUp[] = { 0x088, 0x008, 0x018, 0x024, 0x028, 0x08c };
	const uint32_t usb2cfg = rd32(fWrapper, UKE_USB_DWC3_GUSB2PHYCFG(0));
	const uint32_t usb3pip = rd32(fWrapper, UKE_USB_DWC3_GUSB3PIPECTL(0));
	uint32_t gdscr;

	IOLog("NativeUsbSerial: warm: block reset usb2=%08x usb3=%08x\n", usb2cfg, usb3pip);
	for (uint32_t offset : kDown) {
		wr32(fGcc, offset, rd32(fGcc, offset) & ~UKE_USB_GCC_BRANCH_ENABLE);
	}
	IODelay(100);
	wr32(fGcc, 0x000, rd32(fGcc, 0x000) | UKE_USB_GCC_RESET_ASSERT);
	IODelay(100);
	wr32(fGcc, 0x000, rd32(fGcc, 0x000) & ~UKE_USB_GCC_RESET_ASSERT);
	IODelay(10);
	for (uint32_t offset : kUp) {
		wr32(fGcc, offset, rd32(fGcc, offset) | UKE_USB_GCC_BRANCH_ENABLE);
		IODelay(10);
	}
	IODelay(1000);
	gdscr = rd32(fGcc, 0x004);
	IOLog("NativeUsbSerial: warm: block reset done gdscr=%08x bcr=%08x\n",
	    gdscr, rd32(fGcc, 0x000));

	// The glue page and the core registers reset with the block: put the
	// UTMI-as-pipe routing back and replay the PHY control registers without
	// the suspend bits. No DCTL.CSFTRST here: usbbr2 proved it never
	// self-clears on this core while the device domain lacks its functional
	// clock, and a stuck CSFTRST then freezes the whole domain (DEPCMD,
	// DALEPENA and GEVNTADR writes all stop latching, so every later command
	// "completes" as a dropped write). With the corrected 38.4 MHz PLL table
	// the domain should come up without any soft reset at all.
	uint32_t general = rd32(fQscratch, UKE_USB_QSCRATCH_GENERAL_CFG);
	wr32(fQscratch, UKE_USB_QSCRATCH_GENERAL_CFG,
	    (general & ~UKE_USB_QSCRATCH_PIPE_UTMI_CLK_DIS) |
	    UKE_USB_QSCRATCH_PIPE_UTMI_CLK_SEL |
	    UKE_USB_QSCRATCH_PIPE3_PHYSTATUS_SW);
	IOLog("NativeUsbSerial: warm: general=%08x -> %08x\n",
	    general, rd32(fQscratch, UKE_USB_QSCRATCH_GENERAL_CFG));

	wr32(fWrapper, UKE_USB_DWC3_GUSB2PHYCFG(0), usb2cfg &
	    ~(UKE_USB_DWC3_GUSB2PHYCFG_SUSPHY | (1U << 31)));
	wr32(fWrapper, UKE_USB_DWC3_GUSB3PIPECTL(0), usb3pip &
	    ~UKE_USB_DWC3_GUSB3PIPECTL_SUSPHY);
	IODelay(100);

	uint32_t gctl = rd32(fWrapper, UKE_USB_DWC3_GCTL);
	wr32(fWrapper, UKE_USB_DWC3_GCTL,
	    (gctl & ~UKE_USB_DWC3_GCTL_PRTCAP_MASK) | UKE_USB_DWC3_GCTL_PRTCAP_DEVICE |
	    UKE_USB_DWC3_GCTL_DSBLCLKGTNG);
	IOLog("NativeUsbSerial: warm: gctl -> %08x\n", rd32(fWrapper, UKE_USB_DWC3_GCTL));

	IODelay(100);
	uint32_t id = rd32(fWrapper, UKE_USB_DWC3_GSNPSID);
	IOLog("NativeUsbSerial: warm: replay usb2=%08x usb3=%08x id=%08x\n",
	    rd32(fWrapper, UKE_USB_DWC3_GUSB2PHYCFG(0)),
	    rd32(fWrapper, UKE_USB_DWC3_GUSB3PIPECTL(0)), id);
	if ((id & 0xffff0000U) != 0x33310000U) {
		IOLog("NativeUsbSerial: core did not come back after the block reset\n");
		return false;
	}
	return true;
}

// The usbbr6 read sweep cannot distinguish a dead bridge from a healthy one
// over an untouched tuner space (the init table programs everything through
// MMIO, so the APB registers legitimately read zero). Write the vendor CV9
// pattern through the bridge and read it back: an echo means the bridge and
// the PHY's clock domain are alive and the hunt moves back to the command
// engine; silence means the PHY is unclocked.
void
NativeUsbSerial::phyApbEchoTest(void)
{
	phyApbWrite(0x05, 0xc0);
	phyApbWrite(0x10, 0xa5);
	phyApbRead(0x05, 0xc0);
	phyApbRead(0x10, 0xa5);
}

void
NativeUsbSerial::phyApbWrite(uint8_t reg, uint8_t value)
{
	wr32(fPhy, 0x138, reg);
	wr32(fPhy, 0x13c, value);
	wr32(fPhy, 0x130, (1U << 1) | 1U);
	IODelay(20);
	uint32_t status = rd32(fPhy, 0x134);
	bool busy = (status & (1U << 3)) != 0;
	for (unsigned i = 0; busy && i < 100; i++) {
		IODelay(10);
		status = rd32(fPhy, 0x134);
		busy = (status & (1U << 3)) != 0;
	}
	wr32(fPhy, 0x130, 0);
	IOLog("NativeUsbSerial: warm: phy apb write[%02x]=%02x status=%08x%s\n",
	    reg, value, status, busy ? " BUSY" : "");
}

void
NativeUsbSerial::phyApbRead(uint8_t reg, uint8_t expected)
{
	wr32(fPhy, 0x138, reg);
	wr32(fPhy, 0x130, 1U << 1);
	IODelay(20);
	uint32_t status = rd32(fPhy, 0x134);
	bool busy = (status & (1U << 3)) != 0;
	for (unsigned i = 0; busy && i < 100; i++) {
		IODelay(10);
		status = rd32(fPhy, 0x134);
		busy = (status & (1U << 3)) != 0;
	}
	uint32_t lsb = rd32(fPhy, 0x144) & 0xffU;
	uint32_t msb = rd32(fPhy, 0x148) & 0xffU;
	wr32(fPhy, 0x130, 0);
	IOLog("NativeUsbSerial: warm: phy apb read[%02x]=%02x%02x expected=%02x %s\n",
	    reg, msb, lsb, expected, (msb == 0 && lsb == expected) ? "ECHO" : "no-echo");
}

// USB2 clkref enable through the SCM SIO service (TrustZone performs the
// MMIO from EL3). Every CPU-side access to the TCSR region stalls (usbbr5/
// usbbr7), but the vendor stack itself uses SCM IO for the EUD registers,
// and Android's kernel drives this exact SMC pair constantly. Encoding per
// the vendor qcom_scm.c: ARM_SMCCC fast call, SMC64, SIP owner, FNID
// (SVC_IO 0x05 << 8 | IO_READ 0x01 / IO_WRITE 0x02), arginfo word, then the
// address and value.
void
NativeUsbSerial::scmClkrefProbe(void)
{
	uint64_t ret1 = 0;
	IOLog("NativeUsbSerial: warm: scm io read 1fb1118\n");
	uint64_t ret0 = scmSmc(0xC2020501ULL, 0x1ULL, UKE_USB_TCSR_USB2_CLKREF_EN, 0, &ret1);
	IOLog("NativeUsbSerial: warm: scm read ret=%llx val=%llx\n", ret0, ret1);
	IOLog("NativeUsbSerial: warm: scm io write 1fb1118 <- 1\n");
	ret0 = scmSmc(0xC2020502ULL, 0x2ULL, UKE_USB_TCSR_USB2_CLKREF_EN, 1, nullptr);
	IOLog("NativeUsbSerial: warm: scm write ret=%llx\n", ret0);
	IOLog("NativeUsbSerial: warm: scm io read 1fb1118 (again)\n");
	ret0 = scmSmc(0xC2020501ULL, 0x1ULL, UKE_USB_TCSR_USB2_CLKREF_EN, 0, &ret1);
	IOLog("NativeUsbSerial: warm: scm read ret=%llx val=%llx\n", ret0, ret1);
	IOLog("NativeUsbSerial: warm: apb echo (after scm clkref)\n");
	phyApbEchoTest();
}


// The eUSB2 PHY's PMIC rails are proxy-consumer regulators (pmxr2230 L2B
// 0.88 V "vdd", L4B 1.2 V "vdda12"); ABL's proxy votes are released at the
// kernel handoff and nothing in XNU re-votes them, which is the only state
// difference left against the working pure-warm takeover on SM8550/SM8650
// (vermeer-lab K70, Ace 5). Vote them back on with ACTIVE_ONLY VRM commands
// through apps_rsc drv-2, transcribed from the vendor rpmh stack: cmd-db
// (0x81c60000) supplies the VRM register addresses, the TCS carries the
// message, and AOP aggregates the vote. Voltage/mode first, enable last,
// every command with the completion response requested.
bool
NativeUsbSerial::railsVote(void)
{
	fCmdDb = mapPhys(kUkeCmdDbBase, kUkeCmdDbSize, fCmdDbMap);
	fRsc = mapPhys(kUkeRscDrv2Base, kUkeRscDrv2Size, fRscMap);
	if (!fCmdDb || !fRsc) {
		IOLog("NativeUsbSerial: rails: map failed cmddb=%p rsc=%p\n",
		    reinterpret_cast<void *>(fCmdDb), reinterpret_cast<void *>(fRsc));
		return false;
	}

	const volatile UkeCmdDbHeader *db =
	    reinterpret_cast<const volatile UkeCmdDbHeader *>(fCmdDb);
	if (db->magic[0] != kUkeCmdDbMagic[0] || db->magic[1] != kUkeCmdDbMagic[1] ||
	    db->magic[2] != kUkeCmdDbMagic[2] || db->magic[3] != kUkeCmdDbMagic[3]) {
		IOLog("NativeUsbSerial: rails: cmd-db magic mismatch %02x%02x%02x%02x\n",
		    db->magic[0], db->magic[1], db->magic[2], db->magic[3]);
		return false;
	}
	IOLog("NativeUsbSerial: rails: cmd-db version=%08x standalone=%u\n",
	    db->version, db->reserved & (1u << kUkeCmdDbStandaloneBit) ? 1u : 0u);
	if ((db->reserved & (1u << kUkeCmdDbStandaloneBit)) != 0) {
		IOLog("NativeUsbSerial: rails: cmd-db is standalone; votes are not serviced\n");
		return false;
	}

	const volatile UkeCmdDbRscHdr *vrm = nullptr;
	const volatile UkeCmdDbRscHdr *arc = nullptr;
	for (unsigned i = 0; i < kUkeCmdDbMaxSlv; i++) {
		const uint16_t slv = db->header[i].slvId;
		if (slv == 0) {
			break;
		}
		IOLog("NativeUsbSerial: rails: cmdb block[%u] slv=%u cnt=%u\n",
		    i, slv, db->header[i].count);
		if (slv == kUkeCmdDbHwVrm) {
			vrm = &db->header[i];
		} else if (slv == kUkeCmdDbHwArc) {
			arc = &db->header[i];
		}
	}
	if (vrm == nullptr) {
		IOLog("NativeUsbSerial: rails: no VRM block in cmd-db\n");
		return false;
	}

	// cmd-db ids are 8 bytes and may use separators ("ldo.b2", "xo.lvl");
	// match on the alphanumeric-only projection of both sides.
	auto collapseId = [](const char *id, char *out) {
		unsigned n = 0;
		for (unsigned k = 0; k < 8 && id[k] != '\0'; k++) {
			if ((id[k] >= '0' && id[k] <= '9') || (id[k] >= 'a' && id[k] <= 'z')) {
				out[n++] = id[k];
			}
		}
		out[n] = '\0';
	};
	const uintptr_t dataBase = fCmdDb + offsetof(UkeCmdDbHeader, data);
	uint32_t railAddr[kUkeUsbPhyRailCount] = {};
	uint32_t refClkAddr[sizeof(kUkeUsbPhyRefClocks) / sizeof(kUkeUsbPhyRefClocks[0])] = {};
	char collapsed[9];
	char wanted[9];

	if (vrm->count <= 64) {
		const volatile UkeCmdDbEntry *entries =
		    reinterpret_cast<const volatile UkeCmdDbEntry *>(dataBase + vrm->headerOffset);
		for (unsigned r = 0; r < kUkeUsbPhyRailCount; r++) {
			collapseId(kUkeUsbPhyRails[r].resource, wanted);
			for (unsigned j = 0; j < vrm->count; j++) {
				char id[9];
				for (unsigned k = 0; k < 8; k++) {
					id[k] = static_cast<char>(entries[j].id[k]);
				}
				id[8] = '\0';
				if (j < 12) {
					IOLog("NativeUsbSerial: rails: vrm[%u] %-8s addr=%05x len=%u\n",
					    j, id, entries[j].addr, entries[j].len);
				}
				collapseId(id, collapsed);
				if (railAddr[r] == 0 && strcmp(collapsed, wanted) == 0) {
					railAddr[r] = entries[j].addr;
				}
			}
		}
	} else {
		IOLog("NativeUsbSerial: rails: implausible VRM entry count %u\n", vrm->count);
		return false;
	}

	if (arc != nullptr && arc->count <= 64) {
		const volatile UkeCmdDbEntry *entries =
		    reinterpret_cast<const volatile UkeCmdDbEntry *>(dataBase + arc->headerOffset);
		for (unsigned r = 0; r < sizeof(kUkeUsbPhyRefClocks) / sizeof(kUkeUsbPhyRefClocks[0]); r++) {
			collapseId(kUkeUsbPhyRefClocks[r].resource, wanted);
			for (unsigned j = 0; j < arc->count; j++) {
				char id[9];
				for (unsigned k = 0; k < 8; k++) {
					id[k] = static_cast<char>(entries[j].id[k]);
				}
				id[8] = '\0';
				if (j < 6) {
					IOLog("NativeUsbSerial: rails: arc[%u] %-8s addr=%05x len=%u\n",
					    j, id, entries[j].addr, entries[j].len);
				}
				collapseId(id, collapsed);
				if (refClkAddr[r] == 0 && strcmp(collapsed, wanted) == 0) {
					refClkAddr[r] = entries[j].addr;
				}
			}
		}
	}

	bool haveAll = true;
	for (unsigned r = 0; r < kUkeUsbPhyRailCount; r++) {
		IOLog("NativeUsbSerial: rails: %s addr=%05x\n",
		    kUkeUsbPhyRails[r].resource, railAddr[r]);
		if (railAddr[r] == 0) {
			haveAll = false;
		}
	}
	for (unsigned r = 0; r < sizeof(kUkeUsbPhyRefClocks) / sizeof(kUkeUsbPhyRefClocks[0]); r++) {
		IOLog("NativeUsbSerial: rails: %s addr=%05x\n",
		    kUkeUsbPhyRefClocks[r].resource, refClkAddr[r]);
		if (refClkAddr[r] == 0) {
			haveAll = false;
		}
	}
	if (!haveAll) {
		IOLog("NativeUsbSerial: rails: resource(s) missing from cmd-db; no vote sent\n");
		return false;
	}

	const uint32_t drvId = rd32(fRsc, kUkeRscDrvId);
	const unsigned major = (drvId >> 16) & 0xff;
	const unsigned minor = (drvId >> 8) & 0xff;
	fRscRegs = major >= 3 ? kUkeRscRegsV30 : kUkeRscRegsV27;
	const uint32_t prntChld = rd32(fRsc, fRscRegs.prntChldConfig);
	const unsigned numTcs =
	    (prntChld >> (kUkeDrvNumTcsShift * 2)) & kUkeDrvNumTcsMask;
	const unsigned ncpt = (prntChld >> kUkeDrvNcptShift) & kUkeDrvNcptMask;
	IOLog("NativeUsbSerial: rails: drv2 id=%08x v%u.%u num_tcs=%u ncpt=%u\n",
	    drvId, major, minor, numTcs, ncpt);
	if (ncpt == 0 || ncpt > 16) {
		IOLog("NativeUsbSerial: rails: implausible ncpt; no vote sent\n");
		return false;
	}

	// Offsets inside the drv-2 window; the TCS block starts at +0xd00.
	auto tcsRegOff = [this](uint32_t reg, int tcs) -> uint32_t {
		return kUkeRscDrv2TcsOffset + fRscRegs.tcsStride * static_cast<uint32_t>(tcs) + reg;
	};
	int tcs = -1;
	for (unsigned t = kUkeRscActiveTcsFirst;
	    t < kUkeRscActiveTcsFirst + kUkeRscActiveTcsCount && t < numTcs; t++) {
		if (rd32(fRsc, tcsRegOff(fRscRegs.status, t)) != 0) {
			tcs = static_cast<int>(t);
			break;
		}
	}
	if (tcs < 0) {
		IOLog("NativeUsbSerial: rails: no idle ACTIVE TCS; no vote sent\n");
		return false;
	}
	IOLog("NativeUsbSerial: rails: using ACTIVE tcs %u\n", static_cast<unsigned>(tcs));

	for (unsigned r = 0; r < kUkeUsbPhyRailCount; r++) {
		// Voltage then mode, enable last (rpmh_regulator_reorder_cmds).
		const uint32_t addrs[3] = {
			railAddr[r] + 0, railAddr[r] + 8, railAddr[r] + 4,
		};
		const uint32_t datas[3] = {
			kUkeUsbPhyRails[r].millivolts, kUkeUsbPhyRails[r].mode, 1,
		};
		if (!tcsVoteActive(static_cast<unsigned>(tcs), addrs, datas, 3,
		    kUkeUsbPhyRails[r].resource)) {
			return false;
		}
		IOSleep(2);
	}

	// The reference clock source behind the TCSR gate: ARC "xo.lvl" with the
	// on value 3, the vote clk_prepare_enable(ref_clk_src) sends before the
	// vendor's phy_init programs the PLL for 38.4 MHz.
	const uint32_t refAddrs[1] = { refClkAddr[0] };
	const uint32_t refDatas[1] = { kUkeUsbPhyRefClocks[0].onValue };
	if (!tcsVoteActive(static_cast<unsigned>(tcs), refAddrs, refDatas, 1,
	    kUkeUsbPhyRefClocks[0].resource)) {
		return false;
	}

	// msm_eusb2_phy_power(true) finishes with mb() and EUD_DETECT set at the
	// eud_detect_reg (0xc278000) — "to bring EUD out of reset" once the rails
	// are on; power-down clears it again.
	wr32(fEudDetect, 0, rd32(fEudDetect, 0) | 1U);
	IOLog("NativeUsbSerial: rails: eud detect -> %08x\n", rd32(fEudDetect, 0));
	IOLog("NativeUsbSerial: rails: vote sent (ldob2=880mV ldob4=1200mV enable hpm, xo.lvl=3)\n");
	return true;
}

// One ACTIVE_ONLY message on the given TCS: write every addr/data pair with
// the completion response requested, run the AMC trigger dance from
// __tcs_set_trigger(), poll the TCS back to idle and reclaim it exactly like
// tcs_tx_done() does (no host IRQ is taken; the AOP services the RSC
// interrupt on its side).
bool
NativeUsbSerial::tcsVoteActive(unsigned tcs, const uint32_t *addrs, const uint32_t *datas,
    unsigned count, const char *name)
{
	if (count == 0 || count > 16) {
		return false;
	}
	auto tcsRegOff = [this](uint32_t reg, unsigned tcsId) -> uint32_t {
		return kUkeRscDrv2TcsOffset + fRscRegs.tcsStride * tcsId + reg;
	};
	for (unsigned j = 0; j < count; j++) {
		const uint32_t msgid =
		    kUkeCmdMsgidLen | kUkeCmdMsgidWrite | kUkeCmdMsgidRespReq;
		wr32(fRsc, tcsRegOff(fRscRegs.cmdMsgid, tcs) + fRscRegs.cmdStride * j, msgid);
		wr32(fRsc, tcsRegOff(fRscRegs.cmdAddr, tcs) + fRscRegs.cmdStride * j, addrs[j]);
		wr32(fRsc, tcsRegOff(fRscRegs.cmdData, tcs) + fRscRegs.cmdStride * j, datas[j]);
	}
	uint32_t enable = 0;
	for (unsigned j = 0; j < count; j++) {
		enable |= 1u << j;
	}
	wr32(fRsc, tcsRegOff(fRscRegs.cmdWaitForCmpl, tcs), enable);
	wr32(fRsc, tcsRegOff(fRscRegs.cmdEnable, tcs), enable);

	// __tcs_set_trigger(true): clear trigger, clear mode enable, then enable
	// and trigger, each waiting to stick before the next.
	uint32_t control = rd32(fRsc, tcsRegOff(fRscRegs.control, tcs));
	wr32(fRsc, tcsRegOff(fRscRegs.control, tcs), control & ~kUkeTcsAmcTrigger);
	wr32(fRsc, tcsRegOff(fRscRegs.control, tcs), control & ~(kUkeTcsAmcTrigger | kUkeTcsAmcEnable));
	wr32(fRsc, tcsRegOff(fRscRegs.control, tcs), kUkeTcsAmcEnable);
	wr32(fRsc, tcsRegOff(fRscRegs.control, tcs), kUkeTcsAmcEnable | kUkeTcsAmcTrigger);

	bool idle = false;
	for (unsigned i = 0; i < 500; i++) {
		if (rd32(fRsc, tcsRegOff(fRscRegs.status, tcs)) != 0) {
			idle = true;
			break;
		}
		IOSleep(1);
	}
	uint32_t status[4] = {};
	uint32_t resp[4] = {};
	for (unsigned j = 0; j < count && j < 4; j++) {
		status[j] = rd32(fRsc, tcsRegOff(fRscRegs.cmdStatus, tcs) + fRscRegs.cmdStride * j);
		resp[j] = rd32(fRsc, tcsRegOff(fRscRegs.cmdRespData, tcs) + fRscRegs.cmdStride * j);
	}
	IOLog("NativeUsbSerial: rails: %s vote idle=%u st=%08x/%08x/%08x resp=%08x/%08x/%08x\n",
	    name, idle ? 1u : 0u, status[0], status[1], status[2], resp[0], resp[1], resp[2]);

	// Mirror tcs_tx_done(): give the TCS back and drop the IRQ line.
	wr32(fRsc, tcsRegOff(fRscRegs.cmdEnable, tcs), 0);
	wr32(fRsc, tcsRegOff(fRscRegs.cmdWaitForCmpl, tcs), 0);
	wr32(fRsc, kUkeRscDrv2TcsOffset + fRscRegs.irqClear, 1u << tcs);
	if (!idle) {
		IOLog("NativeUsbSerial: rails: TCS did not return to idle after %s\n", name);
	}
	return idle;
}


bool
NativeUsbSerial::dmaAllocate(void)
{
	IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
	    static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
	    static_cast<IOOptionBits>(kIOMemoryMapperNone);
	fDma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
	    kernel_task, options, kDmaSize, 0xffffffffULL);
	if (fDma == nullptr) {
		IOLog("NativeUsbSerial: DMA allocation failed\n");
		return false;
	}
	fDmaVirt = static_cast<uint8_t *>(fDma->getBytesNoCopy());
	IOByteCount contiguous = 0;
	fDmaPhys = fDma->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
	fDmaSize = kDmaSize;
	if (fDmaVirt == nullptr || fDmaPhys == 0 || contiguous < kDmaSize ||
	    (fDmaPhys & 0xfff) != 0 || fDmaPhys + kDmaSize > 0x100000000ULL) {
		IOLog("NativeUsbSerial: DMA layout invalid virt=%p phys=%llx contiguous=%llu\n",
		    fDmaVirt, static_cast<unsigned long long>(fDmaPhys),
		    static_cast<unsigned long long>(contiguous));
		return false;
	}
	bzero(fDmaVirt, kDmaSize);
	cacheCivac(fDmaVirt, kDmaSize);

	fEvents = reinterpret_cast<UkeUsbEvent *>(fDmaVirt + kEventOffset);
	fEventsPhys = fDmaPhys + kEventOffset;
	fEventHead = 0;

	auto setupEndpoint = [this](Endpoint &ep, uint32_t trbOffset, uint32_t dataSize, uint8_t phys) {
		ep.phys = phys;
		ep.trbs = reinterpret_cast<UkeUsbTrb *>(fDmaVirt + trbOffset);
		ep.trbsPhys = fDmaPhys + trbOffset;
		ep.resource = 0;
		ep.bufferSize = dataSize;
		ep.enabled = false;
		ep.busy = false;
		UkeUsbTrb *link = &ep.trbs[kTrbEntries - 1];
		link->bpl = static_cast<uint32_t>(ep.trbsPhys);
		link->bph = static_cast<uint32_t>(ep.trbsPhys >> 32);
		link->size = 0;
		link->ctrl = UKE_USB_DWC3_TRBCTL_LINK | UKE_USB_DWC3_TRB_HWO;
	};

	setupEndpoint(fEp0Out, kEp0OutTrbOffset, 0, 0);
	setupEndpoint(fEp0In, kEp0InTrbOffset, 0, 1);
	setupEndpoint(fEp1In, kEp1InTrbOffset, 0, 3);
	setupEndpoint(fEp2In, kEp2InTrbOffset, 0, 5);
	setupEndpoint(fEp3Out, kEp3OutTrbOffset, 4096, 6);

	fSetup = fDmaVirt + kSetupOffset;
	fSetupPhys = fDmaPhys + kSetupOffset;
	fStatus = fDmaVirt + kStatusOffset;
	fStatusPhys = fDmaPhys + kStatusOffset;
	fEp0Data = fDmaVirt + kEp0DataOffset;
	fEp0DataPhys = fDmaPhys + kEp0DataOffset;
	fEp2Data = fDmaVirt + kEp2DataOffset;
	fEp2DataPhys = fDmaPhys + kEp2DataOffset;
	fEp3Data = fDmaVirt + kEp3DataOffset;
	fEp3DataPhys = fDmaPhys + kEp3DataOffset;
	return true;
}

bool
NativeUsbSerial::smmuAttach(void)
{
	struct smmu_io io = {};
	io.ctx = this;
	io.rd = &NativeUsbSerial::smmuRead;
	io.wr = &NativeUsbSerial::smmuWrite;
	struct smmu_plan plan = smmu_plan_bypass(&io, UKE_USB_STREAM_ID, fDmaPhys, fDmaSize);
	if (plan.error != nullptr) {
		IOLog("NativeUsbSerial: SMMU plan refused: %s (slot=%u bank=%u)\n",
		    plan.error, plan.slot, plan.bank);
		return false;
	}
	const char *failure = smmu_configure_bypass(&io, &plan, UKE_USB_STREAM_ID);
	if (failure != nullptr) {
		IOLog("NativeUsbSerial: SMMU configure failed: %s\n", failure);
		return false;
	}
	IOLog("NativeUsbSerial: SMMU stream 0x%x -> bypass slot=%u bank=%u\n",
	    UKE_USB_STREAM_ID, plan.slot, plan.bank);
	return true;
}

void
NativeUsbSerial::fifoConfig(void)
{
	// Depth is the low 16 bits and start address the high 16 bits, in
	// 32-bit words: EP0 256 B, EP1 256 B, EP2 512 B, RX 1 KiB.
	wr32(fWrapper, UKE_USB_DWC3_GTXFIFOSIZ(0), 0x00000040);
	wr32(fWrapper, UKE_USB_DWC3_GTXFIFOSIZ(1), 0x00400040);
	wr32(fWrapper, UKE_USB_DWC3_GTXFIFOSIZ(2), 0x00800080);
	wr32(fWrapper, UKE_USB_DWC3_GRXFIFOSIZ(0), 0x00000100);
}

void
NativeUsbSerial::eventInit(void)
{
	bzero(fEvents, 4096);
	cacheCivac(fEvents, 4096);
	wr32(fWrapper, UKE_USB_DWC3_GEVNTADRLO, static_cast<uint32_t>(fEventsPhys));
	wr32(fWrapper, UKE_USB_DWC3_GEVNTADRHI, static_cast<uint32_t>(fEventsPhys >> 32));
	// DWC3_GEVNTSIZ_INTMASK is bit 31 (Linux core.h) and the size field is
	// the low 16 bits; the earlier revision wrote bit 24, a reserved field.
	wr32(fWrapper, UKE_USB_DWC3_GEVNTSIZ, (1U << 31) | 4096);
	wr32(fWrapper, UKE_USB_DWC3_GEVNTCOUNT, 0);
}

void
NativeUsbSerial::deviceInit(void)
{
	// The core was halted and reset in warmInit, so DCFG is writable here.
	IOLog("NativeUsbSerial: serial: dcfg before=%08x dsts=%08x\n",
	    rd32(fWrapper, UKE_USB_DWC3_DCFG), rd32(fWrapper, UKE_USB_DWC3_DSTS));
	// Reference: clear the speed and address fields and let the core run high
	// speed at address 0, as dwc3_gadget_start does for a peripheral.
	uint32_t dcfg = rd32(fWrapper, UKE_USB_DWC3_DCFG);
	dcfg &= ~(7U | (127U << 3));
	if (fForceFs) {
		dcfg |= UKE_USB_DWC3_DCFG_DEVSPD_FS;
	}
	wr32(fWrapper, UKE_USB_DWC3_DCFG, dcfg);
	IOLog("NativeUsbSerial: serial: dcfg=%08x speed=%s gctl=%08x\n",
	    rd32(fWrapper, UKE_USB_DWC3_DCFG), fForceFs ? "full" : "high",
	    rd32(fWrapper, UKE_USB_DWC3_GCTL));
	// Vendor __dwc3_gadget_start: clear GRXTHRCFG.PKTCNTSEL (bit 26 on
	// DWC_usb31) so the core computes the ACK NUMP itself instead of using
	// the programmed packet count.
	wr32(fWrapper, UKE_USB_DWC3_GRXTHRCFG,
	    rd32(fWrapper, UKE_USB_DWC3_GRXTHRCFG) & ~(1U << 26));
	// DEVTEN stays 0 here (the halt cleared it): the vendor reference also
	// configures endpoints with the event machinery off and only enables the
	// events right before RUN. GCTL, GUSB2PHYCFG and GUSB3PIPECTL are left
	// exactly as ABL set them.
}

bool
NativeUsbSerial::depCommand(uint8_t phys, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t *result)
{
	wr32(fWrapper, UKE_USB_DWC3_DEPCMDPAR2(phys), p2);
	wr32(fWrapper, UKE_USB_DWC3_DEPCMDPAR1(phys), p1);
	wr32(fWrapper, UKE_USB_DWC3_DEPCMDPAR0(phys), p0);
	wr32(fWrapper, UKE_USB_DWC3_DEPCMD(phys), cmd | UKE_USB_DWC3_DEPCMD_CMDACT);
	for (unsigned i = 0; i < 100; i++) {
		uint32_t value = rd32(fWrapper, UKE_USB_DWC3_DEPCMD(phys));
		if ((value & UKE_USB_DWC3_DEPCMD_CMDACT) == 0) {
			uint32_t status = UKE_USB_DWC3_DEPCMD_STATUS(value);
			if (result != nullptr) {
				*result = rd32(fWrapper, UKE_USB_DWC3_DEPCMDPAR0(phys));
			}
			if (status != 0) {
				IOLog("NativeUsbSerial: DEP%u cmd=%x status=%u\n", phys, cmd, status);
				return false;
			}
			return true;
		}
		IOSleep(1);
	}
	IOLog("NativeUsbSerial: DEP%u cmd=%x timeout readback=%08x gevt=%08x\n",
	    phys, cmd, rd32(fWrapper, UKE_USB_DWC3_DEPCMD(phys)),
	    rd32(fWrapper, UKE_USB_DWC3_GEVNTCOUNT));
	return false;
}

bool
NativeUsbSerial::configEp(Endpoint &ep, uint8_t type, uint16_t mps, uint32_t fifo, bool in, bool ep0)
{
	uint32_t p0 = UKE_USB_DWC3_DEPCFG_EP_TYPE(type) |
	    UKE_USB_DWC3_DEPCFG_MAX_PACKET_SIZE(mps);
	if (in) {
		p0 |= UKE_USB_DWC3_DEPCFG_FIFO_NUMBER(fifo);
	}
	uint32_t p1 = UKE_USB_DWC3_DEPCFG_EP_NUMBER(ep.phys) |
	    UKE_USB_DWC3_DEPCFG_XFER_COMPLETE_EN |
	    UKE_USB_DWC3_DEPCFG_XFER_NOT_READY_EN;
	if (!depCommand(ep.phys, UKE_USB_DWC3_DEPCMD_SETEPCONFIG, p0, p1, 0, nullptr)) {
		return false;
	}
	ep.enabled = true;
	return true;
}

bool
NativeUsbSerial::startTransfer(Endpoint &ep)
{
	UkeUsbTrb *trb = &ep.trbs[0];
	return depCommand(ep.phys, UKE_USB_DWC3_DEPCMD_STARTTRANSFER |
	    UKE_USB_DWC3_DEPCMD_PARAM(ep.resource),
	    static_cast<uint32_t>(ep.trbsPhys),
	    static_cast<uint32_t>(ep.trbsPhys >> 32), 0, nullptr) &&
	    trb != nullptr;
}

bool
NativeUsbSerial::waitTrb(UkeUsbTrb *trb, unsigned timeoutMs)
{
	for (unsigned i = 0; i < timeoutMs; i++) {
		cacheInvalidate(trb, sizeof(*trb));
		if ((trb->ctrl & UKE_USB_DWC3_TRB_HWO) == 0) {
			return true;
		}
		IOSleep(1);
	}
	return false;
}

void
NativeUsbSerial::ep0Arm(void)
{
	UkeUsbTrb *trb = &fEp0Out.trbs[0];
	trb->bpl = static_cast<uint32_t>(fSetupPhys);
	trb->bph = static_cast<uint32_t>(fSetupPhys >> 32);
	trb->size = 8;
	trb->ctrl = UKE_USB_DWC3_TRBCTL_CONTROL_SETUP |
	    UKE_USB_DWC3_TRB_LST | UKE_USB_DWC3_TRB_IOC | UKE_USB_DWC3_TRB_HWO;
	cacheCivac(trb, sizeof(*trb));
	if (!startTransfer(fEp0Out)) {
		IOLog("NativeUsbSerial: EP0 setup start failed\n");
		fEp0Waiting = false;
		return;
	}
	fEp0Waiting = true;
}

void
NativeUsbSerial::ep0Status(bool in)
{
	Endpoint &ep = in ? fEp0In : fEp0Out;
	UkeUsbTrb *trb = &ep.trbs[0];
	trb->bpl = static_cast<uint32_t>(fStatusPhys);
	trb->bph = static_cast<uint32_t>(fStatusPhys >> 32);
	trb->size = 0;
	trb->ctrl = UKE_USB_DWC3_TRBCTL_CONTROL_STATUS2 |
	    UKE_USB_DWC3_TRB_LST | UKE_USB_DWC3_TRB_IOC | UKE_USB_DWC3_TRB_HWO;
	cacheCivac(trb, sizeof(*trb));
	if (!startTransfer(ep) || !waitTrb(trb, 1000)) {
		IOLog("NativeUsbSerial: EP0 status failed in=%u\n", in ? 1U : 0U);
	}
	ep0Arm();
}

void
NativeUsbSerial::ep0DataIn(const uint8_t *data, uint32_t length)
{
	if (length > 4096) {
		length = 4096;
	}
	if (length != 0) {
		memcpy(fEp0Data, data, length);
		cacheCivac(fEp0Data, length);
	}
	UkeUsbTrb *trb = &fEp0In.trbs[0];
	trb->bpl = static_cast<uint32_t>(fEp0DataPhys);
	trb->bph = static_cast<uint32_t>(fEp0DataPhys >> 32);
	trb->size = length;
	trb->ctrl = UKE_USB_DWC3_TRBCTL_CONTROL_DATA |
	    UKE_USB_DWC3_TRB_LST | UKE_USB_DWC3_TRB_IOC | UKE_USB_DWC3_TRB_HWO;
	cacheCivac(trb, sizeof(*trb));
	if (!startTransfer(fEp0In) || !waitTrb(trb, 1000)) {
		IOLog("NativeUsbSerial: EP0 data-in failed len=%u\n", length);
		ep0Stall();
		return;
	}
	ep0Status(false);
}

bool
NativeUsbSerial::ep0DataOut(uint32_t length)
{
	if (length > 4096) {
		length = 4096;
	}
	UkeUsbTrb *trb = &fEp0Out.trbs[0];
	trb->bpl = static_cast<uint32_t>(fEp0DataPhys);
	trb->bph = static_cast<uint32_t>(fEp0DataPhys >> 32);
	trb->size = length;
	trb->ctrl = UKE_USB_DWC3_TRBCTL_CONTROL_DATA |
	    UKE_USB_DWC3_TRB_LST | UKE_USB_DWC3_TRB_IOC | UKE_USB_DWC3_TRB_HWO;
	cacheCivac(trb, sizeof(*trb));
	if (!startTransfer(fEp0Out) || !waitTrb(trb, 1000)) {
		IOLog("NativeUsbSerial: EP0 data-out failed len=%u\n", length);
		ep0Stall();
		return false;
	}
	cacheInvalidate(fEp0Data, length);
	return true;
}

void
NativeUsbSerial::ep0Stall(void)
{
	depCommand(fEp0Out.phys, UKE_USB_DWC3_DEPCMD_SETSTALL, 0, 0, 0, nullptr);
	IOSleep(1);
	depCommand(fEp0Out.phys, UKE_USB_DWC3_DEPCMD_CLEARSTALL, 0, 0, 0, nullptr);
	ep0Arm();
}

void
NativeUsbSerial::handleSetup(const uint8_t *setup)
{
	const uint8_t requestType = setup[0];
	const uint8_t request = setup[1];
	const uint16_t value = le16(setup + 2);
	const uint16_t index = le16(setup + 4);
	const uint16_t length = le16(setup + 6);
	const uint8_t type = static_cast<uint8_t>(requestType & 0x60U);
	const bool directionIn = (requestType & 0x80U) != 0;

	// The first few setups tell us how far enumeration got when the host
	// reports descriptor timeouts.
	if (fSetupLog < 8) {
		IOLog("NativeUsbSerial: setup rt=%02x req=%02x val=%04x idx=%04x len=%u\n",
		    static_cast<unsigned>(requestType), static_cast<unsigned>(request),
		    static_cast<unsigned>(value), static_cast<unsigned>(index),
		    static_cast<unsigned>(length));
		fSetupLog++;
	}

	if (type == 0x00U) {
		switch (request) {
		case 0x06: { // GET_DESCRIPTOR
			const uint8_t *data = nullptr;
			uint32_t dataLength = 0;
			switch (value >> 8) {
			case 1:
				data = fDescriptors.device;
				dataLength = static_cast<uint32_t>(sizeof(fDescriptors.device));
				break;
			case 2:
				data = fDescriptors.config;
				dataLength = fDescriptors.configLength;
				break;
			case 3:
				switch (value & 0xffU) {
				case 0:
					data = fDescriptors.stringLang;
					dataLength = fDescriptors.stringLangLength;
					break;
				case 1:
					data = fDescriptors.stringManufacturer;
					dataLength = fDescriptors.stringManufacturerLength;
					break;
				case 2:
					data = fDescriptors.stringProduct;
					dataLength = fDescriptors.stringProductLength;
					break;
				case 3:
					data = fDescriptors.stringSerial;
					dataLength = fDescriptors.stringSerialLength;
					break;
				default:
					ep0Stall();
					return;
				}
				break;
			default:
				ep0Stall();
				return;
			}
			if (dataLength > length) {
				dataLength = length;
			}
			if (dataLength == 0) {
				ep0Status(true);
			} else {
				ep0DataIn(data, dataLength);
			}
			return;
		}
		case 0x05: { // SET_ADDRESS
			const uint32_t address = value & 0x7fU;
			uint32_t dcfg = rd32(fWrapper, UKE_USB_DWC3_DCFG);
			dcfg &= ~(0x7fU << 3);
			dcfg |= address << 3;
			wr32(fWrapper, UKE_USB_DWC3_DCFG, dcfg);
			ep0Status(true);
			return;
		}
		case 0x09: // SET_CONFIGURATION
			fConfigured = (value != 0);
			if (fConfigured) {
				configureData();
			} else {
				fSerialReady = false;
			}
			ep0Status(true);
			return;
		case 0x08: { // GET_CONFIGURATION
			uint8_t configured = static_cast<uint8_t>(fConfigured ? 1 : 0);
			ep0DataIn(&configured, 1);
			return;
		}
		case 0x00: { // GET_STATUS
			uint8_t status[2] = { 0, 0 };
			ep0DataIn(status, 2);
			return;
		}
		case 0x01: // CLEAR_FEATURE
		case 0x03: // SET_FEATURE
			ep0Status(true);
			return;
		case 0x0b: // SET_INTERFACE
			ep0Status(true);
			return;
		case 0x0a: { // GET_INTERFACE
			uint8_t alternate = 0;
			ep0DataIn(&alternate, 1);
			return;
		}
		default:
			ep0Stall();
			return;
		}
	}

	if (type == 0x20U) { // CDC class, interface
		switch (request) {
		case 0x20: // SET_LINE_CODING
			if (length >= 7 && ep0DataOut(7)) {
				memcpy(fLineCoding, fEp0Data, 7);
				ep0Status(true);
			} else if (length < 7) {
				ep0Stall();
			}
			return;
		case 0x21: // GET_LINE_CODING
			ep0DataIn(fLineCoding, 7);
			return;
		case 0x22: // SET_CONTROL_LINE_STATE
			ep0Status(true);
			return;
		default:
			ep0Stall();
			return;
		}
	}

	(void)index;
	(void)directionIn;
	ep0Stall();
}

void
NativeUsbSerial::ep0Poll(void)
{
	if (!fEp0Waiting) {
		if (fEp0Out.enabled) {
			ep0Arm();
		}
		return;
	}
	UkeUsbTrb *trb = &fEp0Out.trbs[0];
	// A short poll keeps bulk log delivery moving while the host has no
	// control request pending; the armed TRB stays valid across returns.
	if (!waitTrb(trb, 10)) {
		return;
	}
	fEp0Waiting = false;
	const uint32_t trbStatus = (trb->size >> 28) & 0xfU;
	if (trbStatus != UKE_USB_DWC3_TRB_STS_OK &&
	    trbStatus != UKE_USB_DWC3_TRB_STS_SETUP_PENDING && fSetupLog < 8) {
		IOLog("NativeUsbSerial: ep0 setup trb status=%u size=%08x\n",
		    static_cast<unsigned>(trbStatus), trb->size);
		fSetupLog++;
	}
	if (trbStatus == UKE_USB_DWC3_TRB_STS_SETUP_PENDING) {
		ep0Arm();
		return;
	}
	cacheInvalidate(fSetup, 8);
	handleSetup(fSetup);
}

bool
NativeUsbSerial::configureData(void)
{
	if (fSerialReady) {
		return true;
	}
	if (!configEp(fEp1In, UKE_USB_DWC3_EP_TYPE_INTR, 16, 1, true, false) ||
	    !configEp(fEp2In, UKE_USB_DWC3_EP_TYPE_BULK, 512, 2, true, false) ||
	    !configEp(fEp3Out, UKE_USB_DWC3_EP_TYPE_BULK, 512, 0, false, false)) {
		IOLog("NativeUsbSerial: data endpoint configuration failed\n");
		return false;
	}
	// Endpoints 1..3 carry the notification and both bulk pipes.
	wr32(fWrapper, UKE_USB_DWC3_DALEPENA, rd32(fWrapper, UKE_USB_DWC3_DALEPENA) | 0x0eU);
	IOLog("NativeUsbSerial: data endpoints enabled dalepena=%08x\n",
	    rd32(fWrapper, UKE_USB_DWC3_DALEPENA));
	fSerialReady = true;
	queueBanner("UniXNU uke USB serial\n"
	            "CDC-ACM ready; commands: help, dump, version\n");
	serviceRx();
	return true;
}

void
NativeUsbSerial::queueBanner(const char *text)
{
	if (fLock == nullptr || fTxMem == nullptr || text == nullptr) {
		return;
	}
	const size_t length = strlen(text);
	IOLockLock(fLock);
	fTxRing.writeBytes(text, static_cast<uint32_t>(length), true);
	IOLockUnlock(fLock);
}

void
NativeUsbSerial::serviceEvents(void)
{
	uint32_t count = rd32(fWrapper, UKE_USB_DWC3_GEVNTCOUNT) & 0xffffU;
	if (count == 0) {
		return;
	}
	uint32_t processed = 0;
	while (count-- != 0) {
		UkeUsbEvent *event = &fEvents[fEventHead];
		fEventHead = (fEventHead + 1) % kEventEntries;
		cacheInvalidate(event, sizeof(*event));
		if ((event->info & 1U) == 0) {
			const uint32_t type = (event->info >> 1) & 0x7fU;
			// A host that reports descriptor timeouts means either the reset
			// event never arrived or EP0 never saw the setup packet; this
			// tells the two apart.
			if (fEventLog < 12) {
				IOLog("NativeUsbSerial: event type=%u info=%08x\n",
				    static_cast<unsigned>(type), event->info);
				fEventLog++;
			}
			if (type == UKE_USB_DWC3_DEV_RESET) {
				fConfigured = false;
				fSerialReady = false;
				fEp0Waiting = false;
				fEp2In.busy = false;
				fEp3Out.busy = false;
				if (!configEp(fEp0Out, UKE_USB_DWC3_EP_TYPE_CONTROL, 64, 0, false, true) ||
				    !configEp(fEp0In, UKE_USB_DWC3_EP_TYPE_CONTROL, 64, 0, true, true)) {
					IOLog("NativeUsbSerial: EP0 reconfiguration after reset failed\n");
				}
				wr32(fWrapper, UKE_USB_DWC3_DALEPENA,
				    rd32(fWrapper, UKE_USB_DWC3_DALEPENA) | 1U);
				ep0Arm();
			} else if (type == UKE_USB_DWC3_DEV_DISCONNECT) {
				fConfigured = false;
				fSerialReady = false;
				fEp0Waiting = false;
			} else if (type == UKE_USB_DWC3_DEV_CONNECT_DONE && !fEp0Waiting) {
				ep0Arm();
			}
		}
		processed++;
	}
	wr32(fWrapper, UKE_USB_DWC3_GEVNTCOUNT, processed);
}

void
NativeUsbSerial::serviceTx(void)
{
	if (fEp2In.busy) {
		UkeUsbTrb *trb = &fEp2In.trbs[0];
		cacheInvalidate(trb, sizeof(*trb));
		if ((trb->ctrl & UKE_USB_DWC3_TRB_HWO) != 0) {
			return;
		}
		fEp2In.busy = false;
	}
	if (fLock == nullptr || fTxRing.available() == 0) {
		return;
	}
	uint32_t length = fTxRing.available();
	if (length > kMaxTransfer) {
		length = kMaxTransfer;
	}
	IOLockLock(fLock);
	length = fTxRing.readBytes(fEp2Data, length);
	IOLockUnlock(fLock);
	if (length == 0) {
		return;
	}
	cacheCivac(fEp2Data, length);
	UkeUsbTrb *trb = &fEp2In.trbs[0];
	trb->bpl = static_cast<uint32_t>(fEp2DataPhys);
	trb->bph = static_cast<uint32_t>(fEp2DataPhys >> 32);
	trb->size = length;
	trb->ctrl = UKE_USB_DWC3_TRBCTL_NORMAL |
	    UKE_USB_DWC3_TRB_LST | UKE_USB_DWC3_TRB_IOC | UKE_USB_DWC3_TRB_HWO;
	cacheCivac(trb, sizeof(*trb));
	if (!startTransfer(fEp2In)) {
		IOLog("NativeUsbSerial: bulk-in start failed\n");
		return;
	}
	fEp2In.busy = true;
}

void
NativeUsbSerial::serviceRx(void)
{
	if (fEp3Out.busy) {
		UkeUsbTrb *trb = &fEp3Out.trbs[0];
		cacheInvalidate(trb, sizeof(*trb));
		if ((trb->ctrl & UKE_USB_DWC3_TRB_HWO) != 0) {
			return;
		}
		const uint32_t residual = trb->size & 0xffffffU;
		uint32_t actual = residual <= fEp3Out.bufferSize ? fEp3Out.bufferSize - residual : 0;
		if (actual != 0) {
			cacheInvalidate(fEp3Data, actual);
			for (uint32_t i = 0; i < actual; i++) {
				const uint8_t c = fEp3Data[i];
				if (c == '\r') {
					continue;
				}
				if (c == '\n' || fCmdLength + 1 >= sizeof(fCmd)) {
					fCmd[fCmdLength] = '\0';
					processCommand();
					fCmdLength = 0;
				} else {
					fCmd[fCmdLength++] = c;
				}
			}
		}
		fEp3Out.busy = false;
	}
	if (!fEp3Out.enabled) {
		return;
	}
	UkeUsbTrb *trb = &fEp3Out.trbs[0];
	trb->bpl = static_cast<uint32_t>(fEp3DataPhys);
	trb->bph = static_cast<uint32_t>(fEp3DataPhys >> 32);
	trb->size = fEp3Out.bufferSize;
	trb->ctrl = UKE_USB_DWC3_TRBCTL_NORMAL |
	    UKE_USB_DWC3_TRB_LST | UKE_USB_DWC3_TRB_IOC | UKE_USB_DWC3_TRB_HWO;
	cacheCivac(trb, sizeof(*trb));
	if (startTransfer(fEp3Out)) {
		fEp3Out.busy = true;
	}
}

void
NativeUsbSerial::processCommand(void)
{
	if (fCmdLength == 0) {
		return;
	}
	if (strcmp(reinterpret_cast<const char *>(fCmd), "help") == 0) {
		queueBanner("commands: help, dump, version\n");
	} else if (strcmp(reinterpret_cast<const char *>(fCmd), "dump") == 0) {
		fLogPrimed = false;
		fDump = true;
		queueBanner("msgbuf dump requested\n");
	} else if (strcmp(reinterpret_cast<const char *>(fCmd), "version") == 0) {
		queueBanner("UniXNU NativeUsbSerial 1\n");
	} else {
		queueBanner("unknown command; try help\n");
	}
}

void
NativeUsbSerial::pumpMsgbuf(void)
{
	struct msgbuf *message = msgbufp;
	if (message == nullptr || message->msg_bufc == nullptr ||
	    message->msg_size <= 1 || message->msg_magic != MSG_MAGIC ||
	    message->msg_bufx < 0) {
		return;
	}
	if (!fLogPrimed) {
		fLogCursor.prime(static_cast<uint32_t>(message->msg_size),
		    static_cast<uint32_t>(message->msg_bufx), fDump);
		fLogPrimed = true;
	}
	uint8_t buffer[kLogChunk];
	bsd_log_lock_safe();
	uint32_t length = fLogCursor.take(message->msg_bufc,
	    static_cast<uint32_t>(message->msg_bufx), buffer,
	    static_cast<uint32_t>(sizeof(buffer)));
	bsd_log_unlock();
	if (length == 0 || fLock == nullptr) {
		return;
	}
	IOLockLock(fLock);
	fTxRing.writeBytes(buffer, length, true);
	IOLockUnlock(fLock);
}

void
NativeUsbSerial::serialLoop(void)
{
	while (!fStop) {
		serviceEvents();
		ep0Poll();
		if (fSerialReady) {
			serviceTx();
			serviceRx();
		}
		IOSleep(1);
	}
}

void
NativeUsbSerial::logLoop(void)
{
	while (!fStop) {
		pumpMsgbuf();
		IOSleep(20);
	}
}

void
NativeUsbSerial::usbThreadEntry(void *context, wait_result_t result)
{
	(void)result;
	static_cast<NativeUsbSerial *>(context)->serialLoop();
}

void
NativeUsbSerial::logThreadEntry(void *context, wait_result_t result)
{
	(void)result;
	static_cast<NativeUsbSerial *>(context)->logLoop();
}

bool
NativeUsbSerial::serialStart(void)
{
	fDma = nullptr;
	fDmaVirt = nullptr;
	fDmaPhys = 0;
	fDmaSize = 0;
	fEvents = nullptr;
	fEventsPhys = 0;
	fEventHead = 0;
	fSetup = nullptr;
	fStatus = nullptr;
	fEp0Data = nullptr;
	fEp2Data = nullptr;
	fEp3Data = nullptr;
	fTxMem = nullptr;
	fLock = nullptr;
	fStop = false;
	fSerialStarted = false;
	fConfigured = false;
	fSerialReady = false;
	fEp0Waiting = false;
	fLogPrimed = false;
	fCmdLength = 0;
	bzero(fLineCoding, sizeof(fLineCoding));
	fDescriptors.build();

	if (fEnableRails) {
		IOLog("NativeUsbSerial: serial: rails\n");
		if (!railsVote()) {
			// Not fatal: the pure-warm APB echo below still measures the PHY
			// state and the rest of bring-up reports what works.
			IOLog("NativeUsbSerial: rails vote failed; continuing without it\n");
		}
	}

	if (!warmInit()) {
		IOLog("NativeUsbSerial: warm init failed\n");
		return false;
	}
	IOLog("NativeUsbSerial: serial: dma\n");
	if (!dmaAllocate()) {
		return false;
	}
	IOLog("NativeUsbSerial: serial: smmu\n");
	if (!smmuAttach()) {
		return false;
	}
	IOLog("NativeUsbSerial: serial: event\n");
	eventInit();
	IOLog("NativeUsbSerial: serial: event adr=%08x siz=%08x count=%08x\n",
	    rd32(fWrapper, UKE_USB_DWC3_GEVNTADRLO), rd32(fWrapper, UKE_USB_DWC3_GEVNTSIZ),
	    rd32(fWrapper, UKE_USB_DWC3_GEVNTCOUNT));
	IOLog("NativeUsbSerial: serial: fifo\n");
	fifoConfig();
	IOLog("NativeUsbSerial: serial: device\n");
	deviceInit();

	IOLog("NativeUsbSerial: serial: ep0\n");
	// ABL's fastboot left its endpoints enabled; Set Endpoint Config with
	// ACTION_INIT only runs on a disabled endpoint, which is why it hung on
	// top of the warm state (DEPSTARTCFG needs no endpoint state and ran).
	// Drop every enable first and log what ABL had.
	uint32_t dalepena0 = rd32(fWrapper, UKE_USB_DWC3_DALEPENA);
	wr32(fWrapper, UKE_USB_DWC3_DALEPENA, 0);
	IOLog("NativeUsbSerial: serial: dalepena %08x -> 00000000\n", dalepena0);
	// Linux's canonical order (dwc3_gadget_start_config + ep0 enable):
	// Start New Configuration on ep0-out first, then Set Transfer Resource
	// for every hardware endpoint, and only then the Set Endpoint Config
	// pairs. usbbr1 showed SETEPCONFIG as the first post-DEPSTARTCFG
	// command hanging, and the vendor never issues that order.
	if (!depCommand(0, UKE_USB_DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0, nullptr)) {
		IOLog("NativeUsbSerial: EP0 start-config failed\n");
		return false;
	}
	uint32_t rsc = 0;
	if (!depCommand(0, UKE_USB_DWC3_DEPCMD_SETTRANSFRESOURCE, 1, 0, 0, &rsc)) {
		IOLog("NativeUsbSerial: EP0-out transfer resource failed\n");
		return false;
	}
	fEp0Out.resource = UKE_USB_DWC3_DEPCMD_GET_RSC_IDX(rsc);
	fEp0Out.enabled = true;
	if (!depCommand(1, UKE_USB_DWC3_DEPCMD_SETTRANSFRESOURCE, 1, 0, 0, &rsc)) {
		IOLog("NativeUsbSerial: EP0-in transfer resource failed\n");
		return false;
	}
	fEp0In.resource = UKE_USB_DWC3_DEPCMD_GET_RSC_IDX(rsc);
	fEp0In.enabled = true;
	// The same resource assignment covers the data endpoints: their later
	// Set Endpoint Config (configureData) only configures, matching the
	// vendor loop over every hardware endpoint right after Start New
	// Configuration.
	Endpoint *const dataEps[] = { &fEp1In, &fEp2In, &fEp3Out };
	for (Endpoint *ep : dataEps) {
		uint32_t value = 0;
		if (!depCommand(ep->phys, UKE_USB_DWC3_DEPCMD_SETTRANSFRESOURCE, 1, 0, 0, &value)) {
			return false;
		}
		ep->resource = UKE_USB_DWC3_DEPCMD_GET_RSC_IDX(value);
	}
	if (!configEp(fEp0Out, UKE_USB_DWC3_EP_TYPE_CONTROL, 512, 0, false, true) ||
	    !configEp(fEp0In, UKE_USB_DWC3_EP_TYPE_CONTROL, 512, 0, true, true)) {
		IOLog("NativeUsbSerial: EP0 configuration failed\n");
		return false;
	}
	IOLog("NativeUsbSerial: serial: ep0 done gevt=%08x\n",
	    rd32(fWrapper, UKE_USB_DWC3_GEVNTCOUNT));
	// DALEPENA bit 0 must be set or the controller ignores endpoint 0
	// entirely: the reference driver writes 3 (EP0 + EP1) at this point.
	wr32(fWrapper, UKE_USB_DWC3_DALEPENA, rd32(fWrapper, UKE_USB_DWC3_DALEPENA) | 1U);
	IOLog("NativeUsbSerial: serial: dalepena=%08x\n", rd32(fWrapper, UKE_USB_DWC3_DALEPENA));
	// Arm EP0 before the reconnect: the host answers the pull-up with a bus
	// reset and a setup packet that must find a valid TRB.
	ep0Arm();
	IOLog("NativeUsbSerial: serial: setup\n");

	fTxMem = static_cast<uint8_t *>(IOMalloc(kTxBytes));
	if (fTxMem == nullptr) {
		IOLog("NativeUsbSerial: TX ring allocation failed\n");
		return false;
	}
	fTxRing.init(fTxMem, kTxBytes);
	fLock = IOLockAlloc();
	if (fLock == nullptr) {
		IOLog("NativeUsbSerial: lock allocation failed\n");
		return false;
	}

	IOLog("NativeUsbSerial: serial: threads\n");
	thread_t thread = nullptr;
	if (kernel_thread_start(usbThreadEntry, this, &thread) != KERN_SUCCESS) {
		IOLog("NativeUsbSerial: USB thread failed\n");
		return false;
	}
	thread_deallocate(thread);
	// From here on the polled thread owns the DMA/MMIO pointers; do not free
	// them from stop() even if the log thread fails to start.
	fSerialStarted = true;
	if (kernel_thread_start(logThreadEntry, this, &thread) != KERN_SUCCESS) {
		IOLog("NativeUsbSerial: log thread failed\n");
		return false;
	}
	thread_deallocate(thread);
	IOLog("NativeUsbSerial: serial: started\n");

	// Only now is the polled thread servicing EP0 and the event ring, so this
	// is the first moment a pull-up cycle can survive the host's bus reset and
	// its first setup packet. ABL enumerated its own device on this
	// controller, so the host keeps those descriptors unless it sees a
	// disconnect: drop the USB2 pull-up (DCTL.RUN_STOP), wait, connect again
	// with our DCFG speed. This is dwc3_gadget_pullup()'s sequence.
	IOLog("NativeUsbSerial: serial: run\n");
	// Reference QSCRATCH programming: HS_PHY_CTRL keeps the UTMI clock and the
	// software session-valid bit, SS_PHY_CTRL advertises the lane power that
	// the QMP PHY already has. Both pages are ABL state we only OR into.
	uint32_t hs = rd32(fQscratch, 0x10);
	wr32(fQscratch, 0x10, hs | (1U << 20) | (1U << 28));
	uint32_t ss = rd32(fQscratch, 0x30);
	wr32(fQscratch, 0x30, ss | (1U << 24));
	IOLog("NativeUsbSerial: serial: qscratch hs=%08x ss=%08x\n",
	    rd32(fQscratch, 0x10), rd32(fQscratch, 0x30));
	// Reference timing: the event machinery comes on only now, after every
	// endpoint command has run with DEVTEN at 0.
	wr32(fWrapper, UKE_USB_DWC3_DEVTEN,
	    UKE_USB_DWC3_DEVTEN_DISCONN | UKE_USB_DWC3_DEVTEN_USBRST |
	    UKE_USB_DWC3_DEVTEN_CONNECTDONE | (1U << 9) | (1U << 11));
	uint32_t runDctl = rd32(fWrapper, UKE_USB_DWC3_DCTL);
	wr32(fWrapper, UKE_USB_DWC3_DCTL, runDctl & ~UKE_USB_DWC3_DCTL_RUN_STOP);
	IOSleep(200);
	wr32(fWrapper, UKE_USB_DWC3_DCTL,
	    rd32(fWrapper, UKE_USB_DWC3_DCTL) | UKE_USB_DWC3_DCTL_RUN_STOP);
	IOSleep(200);
	// The bus reset that follows the pull-up discards whatever setup TRB was
	// armed; clearing the flag makes ep0Poll post a fresh one instead of
	// treating the discarded TRB as a completed setup.
	fEp0Waiting = false;
	IOLog("NativeUsbSerial: serial: run dctl=%08x dsts=%08x\n",
	    rd32(fWrapper, UKE_USB_DWC3_DCTL), rd32(fWrapper, UKE_USB_DWC3_DSTS));
	// The host answers a pull-up within about a second with a bus reset; three
	// samples show whether it happened and whether the controller produced any
	// event for it.
	for (unsigned i = 1; i <= 3; i++) {
		IOSleep(1000);
		IOLog("NativeUsbSerial: serial: run+%us dctl=%08x dsts=%08x devten=%08x gevt=%08x\n",
		    i, rd32(fWrapper, UKE_USB_DWC3_DCTL), rd32(fWrapper, UKE_USB_DWC3_DSTS),
		    rd32(fWrapper, UKE_USB_DWC3_DEVTEN), rd32(fWrapper, UKE_USB_DWC3_GEVNTCOUNT));
	}
	// One summary line that always lands at the tail of the screen: the
	// earlier identity lines scroll away during the rest of the boot.
	IOLog("NativeUsbSerial: serial: end id=%08x gctl=%08x dctl=%08x dsts=%08x adr=%08x:%08x siz=%08x cnt=%08x dep=%08x\n",
	    rd32(fWrapper, UKE_USB_DWC3_GSNPSID), rd32(fWrapper, UKE_USB_DWC3_GCTL),
	    rd32(fWrapper, UKE_USB_DWC3_DCTL), rd32(fWrapper, UKE_USB_DWC3_DSTS),
	    rd32(fWrapper, UKE_USB_DWC3_GEVNTADRHI), rd32(fWrapper, UKE_USB_DWC3_GEVNTADRLO),
	    rd32(fWrapper, UKE_USB_DWC3_GEVNTSIZ), rd32(fWrapper, UKE_USB_DWC3_GEVNTCOUNT),
	    rd32(fWrapper, UKE_USB_DWC3_DALEPENA));
	IOLog("NativeUsbSerial: warm USB2 device started\n");
	return true;
}

void
NativeUsbSerial::serialStop(void)
{
	fStop = true;
	// The polled threads may still be sleeping. Keep the DMA/MMIO
	// allocations for this boot instead of freeing memory under them.
	if (!fSerialStarted) {
		// Keep the physically-contiguous descriptor allocated, matching the
		// NativeUfs bring-up pattern; a failed/never-started driver is rare
		// and the buffer is released with the boot.
		fDma = nullptr;
		if (fTxMem != nullptr) {
			IOFree(fTxMem, kTxBytes);
			fTxMem = nullptr;
		}
		if (fLock != nullptr) {
			IOLockFree(fLock);
			fLock = nullptr;
		}
	}
}

IOService *
NativeUsbSerial::probe(IOService *provider, SInt32 *score)
{
	OSData *compatible = OSDynamicCast(OSData, provider->getProperty("compatible"));
	if (compatible == nullptr) {
		return nullptr;
	}
	if (!compatibleHas(compatible, "qcom,dwc-usb3-msm") &&
	    !compatibleHas(compatible, "qcom,sm7675-dwc3") &&
	    !compatibleHas(compatible, "snps,dwc3")) {
		return nullptr;
	}
	if (IOService::probe(provider, score) == nullptr) {
		return nullptr;
	}
	*score = 1000;
	return this;
}

bool
NativeUsbSerial::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}

	uint32_t probeMode = 0;
	uint32_t serialMode = 0;
	uint32_t dumpMode = 0;
	uint32_t phyMode = 0;
	// The TCSR clkref write is opt-in and OFF: at the corrected address
	// (0x1fb1118, tcsrcc + 0xb1118) the store stalls the whole machine
	// (usbbr5), and at the old 0x1f4b118 it landed in the ADSP ipc_hwmutex
	// window and did nothing. The reference clock is expected to arrive
	// held on by the firmware (the CLK_HOLD_STATE model of the uke clock
	// handoff), so the driver leaves TCSR alone unless asked.
	uint32_t tcsrMode = 0;
	uint32_t scmMode = 0;
	PE_parse_boot_argn("native-usb-scm", &scmMode, sizeof(scmMode));
	uint32_t railsMode = 0;
	PE_parse_boot_argn("native-usb-rails", &railsMode, sizeof(railsMode));
	uint32_t qscratchMode = 2;
	// A stuck CSFTRST wedges the whole device domain on this core (usbdal6:
	// the event ring stops latching its address after a stuck attempt), and
	// it never completes with or without the domain clock, so the default is
	// to take over ABL's halted warm state without a reset. =1 re-enables it.
	uint32_t softResetMode = 0;
	PE_parse_boot_argn("native-usb-probe", &probeMode, sizeof(probeMode));
	PE_parse_boot_argn("native-usb-serial", &serialMode, sizeof(serialMode));
	PE_parse_boot_argn("native-usb-serial-dump", &dumpMode, sizeof(dumpMode));
	PE_parse_boot_argn("native-usb-phy", &phyMode, sizeof(phyMode));
	PE_parse_boot_argn("native-usb-tcsr", &tcsrMode, sizeof(tcsrMode));
	PE_parse_boot_argn("native-usb-qscratch", &qscratchMode, sizeof(qscratchMode));
	PE_parse_boot_argn("native-usb-softreset", &softResetMode, sizeof(softResetMode));
	uint32_t blockResetMode = 0;
	PE_parse_boot_argn("native-usb-bcr", &blockResetMode, sizeof(blockResetMode));
	uint32_t fsMode = 0;
	PE_parse_boot_argn("native-usb-fs", &fsMode, sizeof(fsMode));
	fProbeLevel = probeMode;
	fEnablePhy = phyMode != 0;
	fEnableTcsr = tcsrMode != 0;
	fEnableScm = scmMode != 0;
	fEnableRails = railsMode != 0;
	fEnableQscratch = qscratchMode != 0;
	fSoftReset = softResetMode != 0;
	fBlockReset = blockResetMode != 0;
	fForceFs = fsMode != 0;
	// 2 (the default) is core_base + 0xf8800, the address dwc3-qcom maps.
	// 1 keeps the first revision's transcribed 0x0b58800, which aborts, so a
	// boot can still reproduce that failure.
	fQscratchBase = (qscratchMode == 1) ?
	    UKE_USB_QSCRATCH_BASE_BAD : UKE_USB_QSCRATCH_BASE;
	if (probeMode == 0 && serialMode == 0) {
		IOService::stop(provider);
		return false;
	}

	if (!mapWindows()) {
		releaseMaps();
		IOService::stop(provider);
		return false;
	}

	IOLog("NativeUsbSerial: start probe=%u serial=%u\n", probeMode, serialMode);
	if (probeMode != 0) {
		dumpProbe();
	}
	if (serialMode != 0) {
		fDump = dumpMode != 0;
		if (!serialStart()) {
			IOLog("NativeUsbSerial: serial start failed\n");
			serialStop();
			if (!fSerialStarted) {
				releaseMaps();
			}
			IOService::stop(provider);
			return false;
		}
	}

	registerService();
	return true;
}

void
NativeUsbSerial::stop(IOService *provider)
{
	if (fSerialStarted) {
		serialStop();
		// Do not unmap MMIO while the polled threads may still resume.
		IOService::stop(provider);
		return;
	}
	releaseMaps();
	IOService::stop(provider);
}

void
NativeUsbSerial::free(void)
{
	if (!fSerialStarted) {
		releaseMaps();
	}
	IOService::free();
}
