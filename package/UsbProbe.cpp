// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" bool qcom_dwc3_bring_up();
extern "C" void qcom_dwc3_publish();
extern "C" void qcom_dwc3_diag(bool);
extern "C" void qcom_dwc3_static_init() __asm__("__GLOBAL__sub_I_QcomDwc3.cpp");
extern "C" volatile uint32_t native_usb_stats[54];

namespace {
bool hasClass(const char *name) {
    const OSSymbol *symbol = OSSymbol::withCString(name);
    const bool present = symbol && OSMetaClass::getMetaClassWithName(symbol);
    if (symbol) symbol->release();
    return present;
}

bool loadUsbClasses() {
    // AppleUSBXDCI has no personality. The two stock base classes must have registered before our constructors run.
    const auto device = OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBXDCI");
    native_usb_stats[21] = static_cast<uint32_t>(device);
    if (device != kOSReturnSuccess && device != kOSKextReturnStartStopError) return false;
    const auto host = OSKextLoadKextWithIdentifier("com.apple.driver.usb.AppleUSBXHCI");
    native_usb_stats[46] = static_cast<uint32_t>(host);
    return (host == kOSReturnSuccess || host == kOSKextReturnStartStopError) &&
        hasClass("AppleUSBXDCI") &&
        hasClass("AppleUSBXHCI");
}

void probe(void *, wait_result_t) {
    IOSleep(3000);
    if (qcom_dwc3_bring_up()) {
        if (!loadUsbClasses()) native_usb_stats[0] = static_cast<uint32_t>(-9);
        else {
            void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
            if (!handle) native_usb_stats[0] = static_cast<uint32_t>(-10);
            else {
                qcom_dwc3_static_init();
                if (OSMetaClass::postModLoad(handle)) native_usb_stats[0] = static_cast<uint32_t>(-11);
                else qcom_dwc3_publish();
            }
        }
    }
    printf("[usb] uke stage=%d id=%08x smmu=%08x reset=%08x start=%u device=%08x host=%08x\n",
           static_cast<int32_t>(native_usb_stats[0]), native_usb_stats[1], native_usb_stats[7],
           native_usb_stats[14], native_usb_stats[19], native_usb_stats[21], native_usb_stats[46]);
    if (native_usb_stats[0] == 12) qcom_dwc3_diag(false);
    thread_terminate(current_thread());
}
} // namespace

extern "C" void uke_usb_probe() {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("usb", &enabled, sizeof(enabled)) || enabled != 1) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(probe, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
