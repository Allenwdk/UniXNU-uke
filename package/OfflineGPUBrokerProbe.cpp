// SPDX-License-Identifier: MIT
// Explicit offline registration after real IOBSD/transport publication.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_offline_gpu_static_init(void) __asm__("__GLOBAL__sub_I_NativeOfflineGPU.cpp");
extern "C" IOService *native_offline_gpu_create(void);
extern "C" volatile uint32_t native_gpu_broker_stage;
extern "C" volatile int native_gpu_broker_status;
static IOService *activeBroker;
#if NATIVE_ACCELERATOR_ABI_PROBE
extern "C" void native_accelerator_probe(void);
extern "C" void native_original_client_probe(void);
#endif
#if NATIVE_PROVIDER_ABI_PROBE
extern "C" void native_original_provider_probe(IOService *);
#endif
extern "C" __attribute__((noinline)) void native_gpu_broker_checkpoint(void) {__asm__ volatile("" ::: "memory");}
extern "C" void native_offline_gpu_publish(void) {
    unsigned enabled=0;PE_parse_boot_argn("offline-gpu-broker",&enabled,sizeof(enabled));
    if(enabled!=1)return;
    native_gpu_broker_stage=1;void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle){native_gpu_broker_status=kIOReturnNoResources;native_gpu_broker_checkpoint();return;}
    native_offline_gpu_static_init();native_gpu_broker_status=OSMetaClass::postModLoad(handle);
    if(native_gpu_broker_status){native_gpu_broker_checkpoint();return;}
    OSDictionary *matching=IOService::serviceMatching("NativeVirtioConsole");
    IOService *transport=matching?IOService::waitForMatchingService(matching,5000000000ULL):nullptr;
    OSSafeReleaseNULL(matching);
    if(!transport){native_gpu_broker_status=kIOReturnNotFound;native_gpu_broker_checkpoint();return;}
    IOService *broker=native_offline_gpu_create();bool attached=false;
    if(broker && broker->init() && (attached=broker->attach(transport)) && broker->start(transport)) {
        // Only the explicit private test class is published. The incomplete
        // frontend is not matched by original IOAcceleratorES discovery.
        broker->registerService();activeBroker=broker;native_gpu_broker_status=kIOReturnSuccess;native_gpu_broker_stage=3;
    } else {
        if(attached)broker->detach(transport);OSSafeReleaseNULL(broker);native_gpu_broker_status=kIOReturnNotOpen;
    }
    transport->release();
#if NATIVE_ACCELERATOR_ABI_PROBE
    native_original_client_probe();
    native_accelerator_probe();
#endif
#if NATIVE_PROVIDER_ABI_PROBE
    native_original_provider_probe(activeBroker);
#endif
    native_gpu_broker_checkpoint();
}
