// SPDX-License-Identifier: BSD-2-Clause-Patent
// Start the source-built UFS adapter after the original storage family loads (uke, iPadOS 27).
//
// Ported from UniXNU-sheng feat/ipados27-gpu-parity. The sheng-log hand-off is gone: uke runs its own
// UFS log writer (package/UkeLastLog.c) and the board page decides when it starts.
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

// NativeUfs polls the optional board diagnostic during window reads. The
// minimal kernel does not include it; a weak definition keeps the archive
// linkable and is replaced when the diagnostic component is built in.
extern "C" __attribute__((weak)) void boot_progress_poll(void) {}

static const OSMetaClass *storageParent;
static IOService *storageDriver;
static bool attempted;

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
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("SourceStorage: cannot begin class registration");
    source_storage_base_init();
    source_storage_ufs_init();
    source_storage_ram_init();
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess)
        panic("SourceStorage: class registration failed %x", status);

    OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (!provider) panic("SourceStorage: platform root unavailable");
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
