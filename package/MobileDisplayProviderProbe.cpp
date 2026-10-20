// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline kernel-linked class initialization and genuine original client query.
// This does not publish a display, render, or replace a system display list.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_mobile_display_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeMobileDisplay.cpp");
extern "C" IOService *native_mobile_display_create(void);
extern "C" {
volatile uint32_t native_mobile_provider_stage;
volatile int native_mobile_provider_meta_status = -1;
volatile int native_mobile_provider_query_status = -1;
volatile uint64_t native_mobile_provider_object;
volatile uint64_t native_mobile_provider_client;
volatile uint64_t native_mobile_provider_dimensions[2];
volatile uint64_t native_mobile_provider_expected[2];
volatile uint32_t native_mobile_provider_parent_match;
volatile uint32_t native_mobile_provider_released;
void native_mobile_display_provider_probe(void);
__attribute__((noinline)) void native_mobile_display_provider_checkpoint(void);
void native_mobile_display_provider_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
void native_mobile_display_provider_probe(void) {
    // This code is linked into our kernel and therefore registers under its
    // actual kernel owner, after the original parent kext has initialized.
    // Only our object's initializer section is deferred; original bytes stay
    // unchanged. Normal standalone kext loading would initialize dependencies.
    native_mobile_provider_stage = 1;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) { native_mobile_display_provider_checkpoint(); return; }
    native_mobile_display_static_init();
    native_mobile_provider_meta_status = OSMetaClass::postModLoad(handle);
    if (native_mobile_provider_meta_status) { native_mobile_display_provider_checkpoint(); return; }
    native_mobile_provider_stage = 2;
    IOService *provider = native_mobile_display_create();
    native_mobile_provider_object = (uintptr_t)provider;
    if (!provider) { native_mobile_display_provider_checkpoint(); return; }
    native_mobile_provider_parent_match = provider->metaCast("IOMobileFramebuffer") != nullptr;
    PE_Video video = {};
    PE_current_console(&video);
    native_mobile_provider_expected[0] = video.v_width;
    native_mobile_provider_expected[1] = video.v_height;
    native_mobile_provider_stage = 3;
    IOUserClient *client = OSDynamicCast(IOUserClient,
        OSMetaClass::allocClassWithName("IOMobileFramebufferUserClient"));
    native_mobile_provider_client = (uintptr_t)client;
    bool attached = false, started = false;
    if (client && client->init() && (attached = client->attach(provider)) &&
        (started = client->start(provider))) {
        native_mobile_provider_stage = 4;
        uint64_t values[2] = {UINT64_MAX,UINT64_MAX};
        IOExternalMethodArguments arguments = {};
        arguments.scalarOutput = values;
        arguments.scalarOutputCount = 2;
        native_mobile_provider_query_status = client->externalMethod(8,&arguments,nullptr,nullptr,nullptr);
        native_mobile_provider_dimensions[0] = values[0];
        native_mobile_provider_dimensions[1] = values[1];
        native_mobile_provider_stage = 5;
    }
    if (started) client->stop(provider);
    if (attached) client->detach(provider);
    if (client) client->release();
    provider->release();
    native_mobile_provider_released = 1;
    native_mobile_display_provider_checkpoint();
}
}
