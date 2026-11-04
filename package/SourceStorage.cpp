// SPDX-License-Identifier: BSD-2-Clause-Patent
// Start the source-built UFS adapter after the original storage family loads (uke, iPadOS 27).
//
// Ported from UniXNU-sheng feat/ipados27-gpu-parity. The sheng-log hand-off is gone: uke runs its own
// UFS log writer (package/UkeLastLog.c) and the board page decides when it starts.
#include "UkeClassRegistration.h"
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void source_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void source_storage_ufs_init(void) __asm__("__GLOBAL__sub_I_NativeUfs.cpp");
extern "C" void source_storage_ram_init(void) __asm__("__GLOBAL__sub_I_NativeRamBlock.cpp");
extern "C" IOService *native_ufs_source_create(void);
extern "C" bool native_source_root_policies_load(void);
// NativeUfs records how far root start got and the last soft-failure reason.
extern "C" volatile uint32_t native_ufs_root_step;
extern "C" const char *volatile native_ufs_root_failure;
extern "C" uint32_t native_adreno_preflight_ready __attribute__((weak_import));
extern "C" void source_software_aes_init(void) __asm__("__GLOBAL__sub_I_NativeAES.cpp");
extern "C" IOService *native_software_aes_create(void);
extern "C" volatile uint32_t native_aes_kat_status, native_aes_interface_status, native_aes_registered;

// NativeUfs polls the optional board diagnostic during window reads. The
// minimal kernel does not include it; a weak definition keeps the archive
// linkable and is replaced when the diagnostic component is built in.
extern "C" __attribute__((weak)) void boot_progress_poll(void) {}

static const OSMetaClass *storageParent;
static IOService *storageDriver;
static bool attempted;
static IOService *softwareAES;

// Qualify the original parent before registering the complete software provider.
// The 24A446 builder checks every vtable PAC schema and its 0x120 instance extent.
static void start_image4_dependencies(IOService *provider)
{
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOCryptoAcceleratorFamily");
    if (status != kOSReturnSuccess)
        panic("Image4 startup: crypto family load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOAESAccelerator");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    if (!parent || parent->getClassSize() != 0x120)
        panic("Image4 startup: original AES parent ABI is %u", parent ? parent->getClassSize() : 0);

    void *handle = ukeClassBegin(g_kernel_kmod_info.name);
    if (!handle) panic("Image4 startup: cannot begin SoftwareAES class registration");
    source_software_aes_init();
    status = ukeClassEnd(handle);
    if (status != kOSReturnSuccess)
        panic("Image4 startup: SoftwareAES class registration failed %x", status);

    // This board already starts its source UFS/RAM classes through their real
    // factory/init/attach/start sequence. Do the same for AES before relying on
    // asynchronous catalogue matching. NativeAES::start runs its complete KAT
    // and only then calls registerService, so callers see a working provider.
    softwareAES = native_software_aes_create();
    if (!softwareAES) panic("Image4 startup: cannot allocate SoftwareAES");
    const char *step = !softwareAES->init() ? "init" : !softwareAES->attach(provider) ? "attach" :
                       !softwareAES->start(provider) ? "start" : nullptr;
    if (step || native_aes_kat_status != 1 || native_aes_interface_status != 1 || !native_aes_registered)
        panic("Image4 startup: SoftwareAES %s kat=%u interface=%u registered=%u",
              step ? step : "publication failed", native_aes_kat_status,
              native_aes_interface_status, native_aes_registered);
    printf("Image4 startup: SoftwareAES ready, algorithm and interface KAT passed\n");

    // Publish a real DT-backed defaults nub only if the platform did not already
    // publish it. Its parent must be the real platform expert, which supplies
    // IOPlatformDevice resources; the device-tree relationship is preserved.
    IOPlatformExpert *platform = IOService::getPlatform();
    if (!platform) panic("Image4 startup: platform expert unavailable");
    bool defaultsPresent = false;
    OSIterator *children = platform->getChildIterator(gIOServicePlane);
    if (children) {
        IORegistryEntry *child;
        while ((child = OSDynamicCast(IORegistryEntry, children->getNextObject()))) {
            if (!strcmp(child->getName(), "defaults") && OSDynamicCast(IOPlatformDevice, child)) {
                defaultsPresent = true;
                break;
            }
        }
        children->release();
    }
    if (!defaultsPresent) {
        IORegistryEntry *entry = IORegistryEntry::fromPath("/defaults", gIODTPlane);
        IOPlatformDevice *nub = entry ? new IOPlatformDevice : nullptr;
        const bool ready = nub && nub->init(entry, gIODTPlane) && nub->attach(platform);
        if (entry) entry->release();
        if (!ready) panic("Image4 startup: cannot publish real defaults nub");
        nub->registerService();
        nub->release();
    }
    // Loaded security code and policy registration do not prove that its service
    // matched. Re-submit the original, unchanged kext personality via OSKext.
    status = OSKextLoadKextWithIdentifier("com.apple.security.AppleImage4");
    if (status != kOSReturnSuccess)
        panic("Image4 startup: original Image4 matching failed %x", status);
    // Owner startup mounts and authenticates SystemOS before setting dyld's
    // existing skip-Ignition choice. The original policy remains registered;
    // do not start the separate service that requires Apple-only key2212.
    printf("Image4 startup: owner boot dependencies ready, defaults %s\n",
           defaultsPresent ? "present" : "published");
}

// Used only by the deferred source IOBlockStorageDevice metaclass constructor.
// The C++ class name stays unchanged for compiler-authored arm64e discriminators.
extern "C" const OSMetaClass *native_source_storage_parent(void)
{
    if (!storageParent) panic("SourceStorage: metaclass initialized before original family");
    return storageParent;
}

// The Data seed the loader placed in /chosen/memory-map. Its disk starts after UFS, whose store window
// keeps the changed pages of the seed when the board enables persistence.
static void start_data_disk(IOService *provider)
{
    IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    const bool seed = map && (map->getProperty("PackedRAMDisk") || map->getProperty("RAMDisk"));
    OSSafeReleaseNULL(map);
    if (!seed) return;
    IOService *disk = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NativeRamBlock"));
    if (!disk) panic("SourceStorage: cannot allocate the Data disk");
    const char *step = !disk->init() ? "init" : !disk->attach(provider) ? "attach" : !disk->start(provider) ? "start" : nullptr;
    if (step) panic("SourceStorage: Data disk %s failed", step);
    printf("SourceStorage: Data disk started\n");
}

extern "C" void native_source_storage_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("native-source-storage", &enabled, sizeof(enabled)) || enabled != 1 || attempted)
        return;
    attempted = true;
    printf("SourceStorage: GPU external preflight ready=%u before root startup\n",
           &native_adreno_preflight_ready ? __atomic_load_n(&native_adreno_preflight_ready,__ATOMIC_ACQUIRE) : 0u);
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess)
        panic("SourceStorage: IOStorageFamily load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("SourceStorage: original base layout %u differs from source %lu",
              storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));

    // Authenticate policies before registering hardware-facing classes. The
    // firmware hands off the envelope; the trusted anchor is compiled in.
    if (!native_source_root_policies_load())
        panic("SourceStorage: root policy handoff or authentication failed");
    status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
    if (status != kOSReturnSuccess)
        panic("SourceStorage: APFS load failed %x", status);
    void *handle = ukeClassBegin(g_kernel_kmod_info.name);
    if (!handle) panic("SourceStorage: cannot begin class registration");
    source_storage_base_init();
    source_storage_ufs_init();
    source_storage_ram_init();
    status = ukeClassEnd(handle);
    if (status != kOSReturnSuccess)
        panic("SourceStorage: class registration failed %x", status);

    OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (!provider) panic("SourceStorage: platform root unavailable");
    start_image4_dependencies(provider);
    storageDriver = native_ufs_source_create();
    if (!storageDriver) panic("SourceStorage: cannot allocate UFS service");
    const char *step = !storageDriver->init() ? "init" : !storageDriver->attach(provider) ? "attach" :
                       !storageDriver->start(provider) ? "start" : nullptr;
    if (step)
        panic("SourceStorage: UFS %s failed (root step %u: %s)", step, native_ufs_root_step,
              native_ufs_root_failure ? native_ufs_root_failure : "no reason recorded");
    printf("SourceStorage: UFS started through public IOStorageFamily interface\n");
    start_data_disk(provider);
}
