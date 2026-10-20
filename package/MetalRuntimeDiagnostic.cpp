// SPDX-License-Identifier: MIT
// Inspect the original framework. Never publish a device or fake its methods.
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include "MetalRuntimeDiagnosticInputs.h"
static void native_metal_runtime_diagnostic() {
    void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
    if (!metal) _exit(75);
#define ORIGINAL(name) auto name = reinterpret_cast<decltype(&::name)>(dlsym(RTLD_DEFAULT, #name)); if (!name) _exit(76)
    ORIGINAL(objc_getClass);
    ORIGINAL(objc_getMetaClass);
    ORIGINAL(class_getInstanceSize);
    ORIGINAL(class_getInstanceMethod);
    ORIGINAL(class_getSuperclass);
    ORIGINAL(class_getName);
    ORIGINAL(sel_registerName);
    ORIGINAL(method_getTypeEncoding);
    ORIGINAL(method_getImplementation);
#undef ORIGINAL
    auto create = reinterpret_cast<void *(*)()>(dlsym(metal, "MTLCreateSystemDefaultDevice"));
    if (!create) _exit(77);
    Class base = objc_getClass("_MTLDevice"), meta = objc_getMetaClass("_MTLDevice");
    if (!base || !meta || class_getInstanceSize(base) != expected_metal_instance_bytes ||
        strcmp(class_getName(class_getSuperclass(base)), "NSObject")) _exit(78);
    uintptr_t slide = reinterpret_cast<uintptr_t>(create) - expected_metal_create_address;
    unsigned index = 0;
    for (const auto &test : expected_metal_methods) {
        Class cls = test.meta ? meta : base;
        Method method = class_getInstanceMethod(cls, sel_registerName(test.selector));
        if (!method || strcmp(method_getTypeEncoding(method), test.encoding) ||
            reinterpret_cast<uintptr_t>(method_getImplementation(method)) != test.implementation + slide)
            _exit(0x4d000000 | index);
        ++index;
    }
    // Check the actual IOKit service inventory before calling original Metal.
    auto matching = reinterpret_cast<void *(*)(const char *)>(dlsym(RTLD_DEFAULT, "IOServiceMatching"));
    auto services = reinterpret_cast<int (*)(unsigned, void *, unsigned *)>(dlsym(RTLD_DEFAULT, "IOServiceGetMatchingServices"));
    auto next = reinterpret_cast<unsigned (*)(unsigned)>(dlsym(RTLD_DEFAULT, "IOIteratorNext"));
    auto release = reinterpret_cast<int (*)(unsigned)>(dlsym(RTLD_DEFAULT, "IOObjectRelease"));
    if (!matching || !services || !next || !release) _exit(79);
    void *dictionary = matching("IOAcceleratorES");
    if (!dictionary) _exit(80);
    unsigned iterator = 0;
    int query_result = services(0, dictionary, &iterator);
    if (query_result) _exit(query_result);
    // XNU's actual matching path returns KERN_SUCCESS with a null iterator
    // when IOService::getMatchingServices has no matching objects.
    if (iterator) {
        unsigned service = next(iterator);
        if (service) { release(service); release(iterator); _exit(82); }
        if (release(iterator)) _exit(83);
    }
    if (create() != nullptr) _exit(84);
    // The observed nil device is an explicit missing-provider result, never
    // success for Adreno/Metal rendering. All original code remains untouched.
    _exit(74);
}

int main() {
    if (getpid() != 1 || getpagesize() != 4096 || sysconf(_SC_PAGESIZE) != 4096) _exit(61);
    native_metal_runtime_diagnostic();
    _exit(85);
}
