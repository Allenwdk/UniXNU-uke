// SPDX-License-Identifier: BSD-2-Clause-Patent
// Negative geometry contract only; this process renders no pixels.
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>
struct FramebufferRect {double x,y,width,height;};
static_assert(sizeof(FramebufferRect)==32,"Qualified ARM64 CGFramebufferRect HFA input");
static uint64_t result[16];
int main() {
    result[0]=0x46424745;result[1]=1;result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);
    result[6]=result[9]=result[11]=result[12]=UINT64_MAX;
    void *library=dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer",RTLD_NOW|RTLD_LOCAL);
    result[4]=library!=nullptr;
    auto get=library?reinterpret_cast<int (*)(void **)>(dlsym(library,"IOMobileFramebufferGetMainDisplay")):nullptr;
    auto begin=library?reinterpret_cast<int (*)(void *,uint32_t *)>(dlsym(library,"IOMobileFramebufferSwapBegin")):nullptr;
    auto set=library?reinterpret_cast<int (*)(void *,uint32_t,IOSurfaceRef,FramebufferRect,FramebufferRect,uint32_t)>(dlsym(library,"IOMobileFramebufferSwapSetLayer")):nullptr;
    auto end=library?reinterpret_cast<int (*)(void *)>(dlsym(library,"IOMobileFramebufferSwapEnd")):nullptr;
    result[5]=get && begin && set && end;
    if(result[5]) {
        void *display=nullptr;result[6]=(uint32_t)get(&display);result[7]=(uintptr_t)display;
        if(!result[6] && display) {
            const void *keys[]={kIOSurfaceWidth,kIOSurfaceHeight,kIOSurfaceBytesPerElement,kIOSurfaceBytesPerRow,kIOSurfaceAllocSize,kIOSurfacePixelFormat};
            int32_t values[]={800,600,4,3200,1920000,0x42475241};CFNumberRef numbers[6]={};
            for(unsigned i=0;i<6;i++)numbers[i]=CFNumberCreate(kCFAllocatorDefault,kCFNumberSInt32Type,&values[i]);
            CFDictionaryRef properties=CFDictionaryCreate(kCFAllocatorDefault,keys,reinterpret_cast<const void **>(numbers),6,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            IOSurfaceRef surface=properties?IOSurfaceCreate(properties):nullptr;
            if(properties)CFRelease(properties);for(CFNumberRef n:numbers)if(n)CFRelease(n);
            if(surface) {
                result[8]=IOSurfaceGetID(surface);uint32_t token=0;
                result[9]=(uint32_t)begin(display,&token);result[10]=token;
                if(!result[9]) {
                    result[13]=799;result[14]=800;
                    result[11]=(uint32_t)set(display,0,surface,{0,0,799,600},{0,0,800,600},0);
                    if(!result[11])result[12]=(uint32_t)end(display);
                    result[15]=1;
                }
            }
        }
    }
    register const uint64_t *pointer __asm__("x19")=result;
    __asm__ volatile("brk #0" : : "r"(pointer) : "memory");
    __builtin_unreachable();
}
