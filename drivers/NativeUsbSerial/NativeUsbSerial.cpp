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

	uintptr_t fWrapper = 0;
	uintptr_t fQscratch = 0;
	uintptr_t fPhy = 0;
	uintptr_t fEudEnable = 0;
	uintptr_t fEudDetect = 0;
	uintptr_t fGcc = 0;
	uintptr_t fPhyBcr = 0;
	uintptr_t fTcsr = 0;
	uintptr_t fSmmu = 0;

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

	uint8_t *fTxMem = nullptr;
	UkeUsbByteRing fTxRing;
	IOLock *fLock = nullptr;
	UkeUsbLogCursor fLogCursor;

	uint8_t fCmd[160] = {};
	uint32_t fCmdLength = 0;

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
	fWrapper = fQscratch = fPhy = fEudEnable = fEudDetect = 0;
	fGcc = fPhyBcr = fTcsr = fSmmu = 0;
}

bool
NativeUsbSerial::mapWindows(void)
{
	fWrapper = mapPhys(UKE_USB_WRAPPER_BASE, UKE_USB_WRAPPER_SIZE, fWrapperMap);
	fQscratch = mapPhys(UKE_USB_QSCRATCH_BASE, UKE_USB_QSCRATCH_SIZE, fQscratchMap);
	fPhy = mapPhys(UKE_USB_PHY_BASE, UKE_USB_PHY_SIZE, fPhyMap);
	fEudEnable = mapPhys(UKE_USB_EUD_ENABLE_BASE, 0x10, fEudEnableMap);
	fEudDetect = mapPhys(UKE_USB_EUD_DETECT_BASE, 0x10, fEudDetectMap);
	fGcc = mapPhys(UKE_USB_GDSC_BASE, UKE_USB_GDSC_SIZE, fGccMap);
	fPhyBcr = mapPhys(UKE_USB_GCC_BASE + 0x12000ULL, 0x1000, fPhyBcrMap);
	fTcsr = mapPhys(0x01f4b000ULL, 0x1000, fTcsrMap);
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
	IOLog("NativeUsbSerial: PHY 19.2MHz table: ctrl2=%02x ctrl3=%02x override=%02x\n",
	    kUkeUsbPhyInit19Mhz[7].value, kUkeUsbPhyInit19Mhz[8].value,
	    kUkeUsbPhyInit19Mhz[16].value);
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
	IOLog("NativeUsbSerial: probing uke USB2 wrapper base=%08x\n",
	    static_cast<unsigned>(UKE_USB_WRAPPER_BASE));
	IOLog("NativeUsbSerial: GCC gdscr=%08x cfg=%08x master=%08x sleep=%08x utmi=%08x cfg_noc=%08x aggre=%08x\n",
	    rd32(fGcc, 0x004), rd32(fGcc, 0x008), rd32(fGcc, 0x018),
	    rd32(fGcc, 0x024), rd32(fGcc, 0x028), rd32(fGcc, 0x088),
	    rd32(fGcc, 0x08c));
	IOLog("NativeUsbSerial: GCC usb30-bcr=%08x qusb2phy-bcr=%08x tcsr-usb2-clkref=%08x\n",
	    rd32(fGcc, 0x000), rd32(fPhyBcr, 0x000), rd32(fTcsr, 0x118));

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
	IOLog("NativeUsbSerial: QSCRATCH general=%08x hsphy=%08x\n",
	    rd32(fQscratch, 0x08), rd32(fQscratch, 0x10));

	dumpPhy();
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
	uint32_t gdscr = rd32(fGcc, 0x004);
	if ((gdscr & UKE_USB_GDSCR_HW_CONTROL) != 0) {
		IOLog("NativeUsbSerial: GDSC is hardware-controlled (gdscr=%08x)\n", gdscr);
		return false;
	}
	wr32(fGcc, 0x004, gdscr & ~UKE_USB_GDSCR_SW_COLLAPSE);
	IODelay(1);
	bool powered = false;
	for (unsigned i = 0; i < UKE_USB_GDSC_TIMEOUT_US; i++) {
		if ((rd32(fGcc, 0x008) & UKE_USB_GDSCR_CFG_POWER_UP) != 0) {
			powered = true;
			break;
		}
		IODelay(1);
	}
	if (!powered) {
		IOLog("NativeUsbSerial: GDSC did not power up (cfg=%08x)\n", rd32(fGcc, 0x008));
		return false;
	}

	// USB30 master/sleep/UTMI/cfg-noc/aggre branches, then deassert the
	// USB30 block reset and pulse the eUSB2 PHY reset.
	static const uint32_t branches[] = { 0x018, 0x024, 0x028, 0x088, 0x08c };
	for (uint32_t offset : branches) {
		wr32(fGcc, offset, rd32(fGcc, offset) | UKE_USB_GCC_BRANCH_ENABLE);
	}
	wr32(fGcc, 0x000, rd32(fGcc, 0x000) & ~UKE_USB_GCC_RESET_ASSERT);
	wr32(fPhyBcr, 0x000, 1);
	IODelay(100);
	wr32(fPhyBcr, 0x000, 0);
	IODelay(100);
	wr32(fTcsr, 0x118, rd32(fTcsr, 0x118) | 1);
	IODelay(100);

	for (size_t i = 0; i < UKE_USB_PHY_INIT_COUNT; i++) {
		const UkeUsbPhyWrite &write = kUkeUsbPhyInit19Mhz[i];
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

	uint32_t general = rd32(fQscratch, 0x08);
	general |= 1U;
	general &= ~(1U << 8);
	wr32(fQscratch, 0x08, general);
	uint32_t hs = rd32(fQscratch, 0x10);
	hs |= (1U << 20) | (1U << 28);
	wr32(fQscratch, 0x10, hs);

	uint32_t dctl = rd32(fWrapper, UKE_USB_DWC3_DCTL);
	wr32(fWrapper, UKE_USB_DWC3_DCTL, dctl | UKE_USB_DWC3_DCTL_CSFTRST);
	IODelay(10);
	wr32(fWrapper, UKE_USB_DWC3_DCTL, rd32(fWrapper, UKE_USB_DWC3_DCTL) & ~UKE_USB_DWC3_DCTL_CSFTRST);
	for (unsigned i = 0; i < 1000; i++) {
		if ((rd32(fWrapper, UKE_USB_DWC3_DCTL) & UKE_USB_DWC3_DCTL_CSFTRST) == 0) {
			return true;
		}
		IOSleep(1);
	}
	IOLog("NativeUsbSerial: DWC3 soft reset did not complete\n");
	return false;
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
	wr32(fWrapper, UKE_USB_DWC3_GEVNTSIZ, (1U << 31) | 4096);
	wr32(fWrapper, UKE_USB_DWC3_GEVNTCOUNT, 0);
}

void
NativeUsbSerial::deviceInit(void)
{
	wr32(fWrapper, UKE_USB_DWC3_GCTL,
	    rd32(fWrapper, UKE_USB_DWC3_GCTL) | UKE_USB_DWC3_GCTL_DSBLCLKGTNG);
	wr32(fWrapper, UKE_USB_DWC3_GUSB2PHYCFG(0),
	    rd32(fWrapper, UKE_USB_DWC3_GUSB2PHYCFG(0)) &
	    ~(UKE_USB_DWC3_GUSB2PHYCFG_SUSPHY | UKE_USB_DWC3_GUSB2PHYCFG_ENBLSLPM));
	wr32(fWrapper, UKE_USB_DWC3_GUSB3PIPECTL(0),
	    rd32(fWrapper, UKE_USB_DWC3_GUSB3PIPECTL(0)) |
	    UKE_USB_DWC3_GUSB3PIPECTL_SUSPHY | UKE_USB_DWC3_GUSB3PIPECTL_DEP1P2P3_EN);
	wr32(fWrapper, UKE_USB_DWC3_DCFG,
	    UKE_USB_DWC3_DCFG_DEVSPD_HS | UKE_USB_DWC3_DCFG_DEVADDR(0));
	wr32(fWrapper, UKE_USB_DWC3_DEVTEN,
	    UKE_USB_DWC3_DEVTEN_DISCONN | UKE_USB_DWC3_DEVTEN_USBRST |
	    UKE_USB_DWC3_DEVTEN_CONNECTDONE | UKE_USB_DWC3_DEVTEN_ULSTCNG);
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
	IOLog("NativeUsbSerial: DEP%u cmd=%x timeout\n", phys, cmd);
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
	    UKE_USB_DWC3_DEPCFG_XFER_IN_PROGRESS_EN;
	if (ep0) {
		p1 |= UKE_USB_DWC3_DEPCFG_XFER_NOT_READY_EN;
	}
	if (!depCommand(ep.phys, UKE_USB_DWC3_DEPCMD_SETEPCONFIG, p0, p1, 0, nullptr)) {
		return false;
	}
	uint32_t result = 0;
	if (!depCommand(ep.phys, UKE_USB_DWC3_DEPCMD_SETTRANSFRESOURCE, 1, 0, 0, &result)) {
		return false;
	}
	ep.resource = UKE_USB_DWC3_DEPCMD_GET_RSC_IDX(result);
	if (ep0 && ep.phys == 0) {
		if (!depCommand(ep.phys, UKE_USB_DWC3_DEPCMD_DEPSTARTCFG |
		    UKE_USB_DWC3_DEPCMD_PARAM(ep.resource), 0, 0, 0, nullptr)) {
			return false;
		}
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
	if (((trb->size >> 28) & 0xfU) == UKE_USB_DWC3_TRB_STS_SETUP_PENDING) {
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
	fEp1Data = nullptr;
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

	if (!warmInit()) {
		IOLog("NativeUsbSerial: warm init failed\n");
		return false;
	}
	if (!dmaAllocate()) {
		return false;
	}
	if (!smmuAttach()) {
		return false;
	}
	eventInit();
	fifoConfig();
	deviceInit();

	if (!configEp(fEp0Out, UKE_USB_DWC3_EP_TYPE_CONTROL, 64, 0, false, true) ||
	    !configEp(fEp0In, UKE_USB_DWC3_EP_TYPE_CONTROL, 64, 0, true, true)) {
		IOLog("NativeUsbSerial: EP0 configuration failed\n");
		return false;
	}
	wr32(fWrapper, UKE_USB_DWC3_DCTL,
	    rd32(fWrapper, UKE_USB_DWC3_DCTL) | UKE_USB_DWC3_DCTL_RUN_STOP);
	ep0Arm();

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
	PE_parse_boot_argn("native-usb-probe", &probeMode, sizeof(probeMode));
	PE_parse_boot_argn("native-usb-serial", &serialMode, sizeof(serialMode));
	PE_parse_boot_argn("native-usb-serial-dump", &dumpMode, sizeof(dumpMode));
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
