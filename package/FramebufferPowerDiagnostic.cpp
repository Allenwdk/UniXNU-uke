// SPDX-License-Identifier: BSD-2-Clause-Patent
// Request the original On state through the unchanged public API, offline only.
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>
static uint64_t result[16];
int main() {
    result[0]=0x46425057;result[1]=1;result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);
    result[6]=result[8]=UINT64_MAX;
    void *library=dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer",RTLD_NOW|RTLD_LOCAL);
    result[4]=library!=nullptr;
    auto get=library?reinterpret_cast<int (*)(void **)>(dlsym(library,"IOMobileFramebufferGetMainDisplay")):nullptr;
    auto request=library?reinterpret_cast<int (*)(void *,uint32_t)>(dlsym(library,"IOMobileFramebufferRequestPowerChange")):nullptr;
    result[5]=get && request;
    if(result[5]) {
        void *display=nullptr;result[6]=(uint32_t)get(&display);result[7]=(uintptr_t)display;
        if(!result[6] && display) {
            result[9]=1;result[10]=1;
            result[8]=(uint32_t)request(display,1);result[10]=2;
            uint64_t before=0,after=0;
            __asm__ volatile("mrs %0, cntvct_el0" : "=r"(before));
            result[11]=(uint32_t)usleep(1000000);
            __asm__ volatile("mrs %0, cntvct_el0" : "=r"(after));
            result[12]=after-before;result[10]=3;
        }
    }
    register const uint64_t *pointer __asm__("x19")=result;
    __asm__ volatile("brk #0" : : "r"(pointer) : "memory");
    __builtin_unreachable();
}
