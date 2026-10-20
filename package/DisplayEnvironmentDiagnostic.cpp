// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit offline original-runtime query instrument. No capability overrides.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
static uint64_t result[16];
static uint32_t bits(float value) {uint32_t word;memcpy(&word,&value,sizeof(word));return word;}
int main() {
    result[0]=0x4d47514b;result[1]=1;
    result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);
    void *library=dlopen("/usr/lib/libMobileGestalt.dylib",RTLD_NOW|RTLD_LOCAL);
    result[4]=library!=nullptr;
    using Query=float (*)(CFStringRef,float);
    Query query=library?reinterpret_cast<Query>(dlsym(library,"MGGetFloat32Answer")):nullptr;
    result[5]=query!=nullptr;
    float defaults[]={1.0f,2.0f,7.0f};
    if(query)for(unsigned index=0;index<3;++index) {
        result[6+index]=bits(query(CFSTR("main-screen-scale"),defaults[index]));
    }
    for(unsigned index=0;index<3;++index)result[12+index]=bits(defaults[index]);
    // Keep the actual report pointer in a saved EL0 register at this owned
    // observation boundary. The debugger reads it without changing results.
    register const uint64_t *pointer __asm__("x19")=result;
    __asm__ volatile("brk #0" : : "r"(pointer) : "memory");
    __builtin_unreachable();
}
