// SPDX-License-Identifier: GPL-3.0-or-later
// 24A446 CoreBrightness selects frame-synchronous DBV for the original iPad
// profile. Uke exposes brightness through the independent IOKit backlight
// service. Use CoreBrightness's direct path for its integrated display.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#import <stdlib.h>
#import <string.h>

static BOOL (*originalSyncBrightness)(id, SEL, id);

static BOOL useSyncBrightness(id self, SEL command, id display) {
    if ([display isKindOfClass:[NSNumber class]] &&
        [(NSNumber *)display unsignedLongLongValue] == 0) {
        return NO;
    }
    return originalSyncBrightness(self, command, display);
}

__attribute__((constructor)) static void selectBrightnessTransactions(void) {
    if (strcmp(getprogname(), "backboardd")) return;
    @autoreleasepool {
        dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness", RTLD_NOW);
        Class control = objc_getClass("BLControl");
        Method method = class_getInstanceMethod(control,
            sel_registerName("useSyncBrightnessTransactionForDisplay:"));
        char argument[16] = "", result[16] = "";
        if (method) {
            method_getArgumentType(method, 2, argument, sizeof(argument));
            method_getReturnType(method, result, sizeof(result));
        }
        if (!method || method_getNumberOfArguments(method) != 3 ||
            argument[0] != '@' || (result[0] != 'B' && result[0] != 'c')) {
            NSLog(@"[Uke backlight] brightness transaction ABI mismatch; inactive");
            return;
        }
        originalSyncBrightness = (void *)method_setImplementation(method, (IMP)useSyncBrightness);
        NSLog(@"[Uke backlight] CoreBrightness direct transactions selected for integrated display");
    }
}
