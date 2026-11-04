/* SPDX-License-Identifier: MIT */
#pragma once
#include <libkern/c++/OSMetaClass.h>

// The 27-line source kernel orders deferred registrations with OSKext loads.
// Older kernels retain their existing registration path.
extern "C" void *uke_kext_class_begin(const char *) __attribute__((weak_import));
extern "C" OSReturn uke_kext_class_end(void *) __attribute__((weak_import));
static inline void *ukeClassBegin(const char *identifier) {
    if (&uke_kext_class_begin && &uke_kext_class_end) return uke_kext_class_begin(identifier);
    return OSMetaClass::preModLoad(identifier);
}
static inline OSReturn ukeClassEnd(void *handle) {
    if (&uke_kext_class_begin && &uke_kext_class_end) return uke_kext_class_end(handle);
    return OSMetaClass::postModLoad(handle);
}
