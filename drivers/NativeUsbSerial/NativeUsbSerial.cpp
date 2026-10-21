// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeUsbSerial: uke (SM7675) USB2 peripheral debug probe.
//
// This revision is the read-only bring-up probe. It maps the USB wrapper,
// QSCRATCH, eUSB2 PHY, GCC/GDSC, TCSR and apps-SMMU windows and prints what
// ABL/UEFI left behind. No register is written, no DMA is allocated and no
// thread is started. The CDC-ACM/log path is added in the next revision.
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

#include "UkeUsbBoard.h"
#include "UkeUsbGcc.h"
#include "UkeUsbDwc3.h"
#include "UkeUsbPhy.h"

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

bool
compatibleHas(const OSData *data, const char *wanted)
{
	const char *bytes = static_cast<const char *>(data->getBytesNoCopy());
	const uint32_t length = data->getLength();
	const size_t wantedLength = strlen(wanted);
	if (bytes == nullptr || wantedLength == 0 || length < wantedLength) {
		return false;
	}
	for (uint32_t at = 0; at + wantedLength <= length; at++) {
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

	void releaseMaps(void);
	bool mapWindows(void);
	void dumpProbe(void);
	void dumpSmmu(void);
	void dumpPhy(void);

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
	fWrapper = fQscratch = fPhy = fEudEnable = fEudDetect = fGcc = fPhyBcr = fTcsr = fSmmu = 0;
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
	if (super::probe(provider, score) == nullptr) {
		return nullptr;
	}
	*score = 1000;
	return this;
}

bool
NativeUsbSerial::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}

	uint32_t probeMode = 0;
	uint32_t serialMode = 0;
	PE_parse_boot_argn("native-usb-probe", &probeMode, sizeof(probeMode));
	PE_parse_boot_argn("native-usb-serial", &serialMode, sizeof(serialMode));
	if (probeMode == 0 && serialMode == 0) {
		super::stop(provider);
		return false;
	}

	if (!mapWindows()) {
		releaseMaps();
		super::stop(provider);
		return false;
	}

	IOLog("NativeUsbSerial: start probe=%u serial=%u\n", probeMode, serialMode);
	dumpProbe();
	if (serialMode != 0) {
		IOLog("NativeUsbSerial: serial=1 requested; warm CDC-ACM path is not enabled in this probe revision\n");
	}

	registerService();
	return true;
}

void
NativeUsbSerial::stop(IOService *provider)
{
	releaseMaps();
	super::stop(provider);
}

void
NativeUsbSerial::free(void)
{
	releaseMaps();
	super::free();
}
