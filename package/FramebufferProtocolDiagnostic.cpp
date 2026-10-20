// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise unchanged original framebuffer marshaling. No rendered-frame claim.
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
struct Size {double width,height;};
static_assert(sizeof(Size)==16,"Qualified original ARM64 CGSize output");
static uint64_t result[16];
int main() {
    result[0]=0x46425054;result[1]=1;result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);
    result[6]=result[8]=result[11]=result[13]=UINT64_MAX;result[14]=1;
    void *library=dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer",RTLD_NOW|RTLD_LOCAL);
    result[4]=library!=nullptr;
    using Main=int (*)(void **);using Geometry=int (*)(void *,Size *);
    using Begin=int (*)(void *,uint32_t *);using End=int (*)(void *);
    Main mainDisplay=library?reinterpret_cast<Main>(dlsym(library,"IOMobileFramebufferGetMainDisplay")):nullptr;
    Geometry geometry=library?reinterpret_cast<Geometry>(dlsym(library,"IOMobileFramebufferGetDisplaySize")):nullptr;
    Begin begin=library?reinterpret_cast<Begin>(dlsym(library,"IOMobileFramebufferSwapBegin")):nullptr;
    End end=library?reinterpret_cast<End>(dlsym(library,"IOMobileFramebufferSwapEnd")):nullptr;
    result[5]=mainDisplay && geometry && begin && end;
    if(result[5]) {
        void *display=nullptr;result[6]=(uint32_t)mainDisplay(&display);result[7]=(uintptr_t)display;result[14]=2;
        if(!result[6] && display) {
            Size size={};result[8]=(uint32_t)geometry(display,&size);
            memcpy(&result[9],&size.width,8);memcpy(&result[10],&size.height,8);result[14]=3;
            if(!result[8]) {
                uint32_t token=0;result[11]=(uint32_t)begin(display,&token);result[12]=token;result[14]=4;
#if FRAMEBUFFER_PROTOCOL_EMPTY_END
                if(!result[11]) {result[13]=(uint32_t)end(display);result[14]=5;}
#endif
            }
        }
    }
    register const uint64_t *pointer __asm__("x19")=result;
    __asm__ volatile("brk #0" : : "r"(pointer) : "memory");
    __builtin_unreachable();
}
