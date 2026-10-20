/* SPDX-License-Identifier: MIT */
#ifdef NDEBUG
#error Native VPC controls require active assertions
#endif
#ifndef MESA_REFERENCE
#error Native VPC controls require the actual primary reference helpers
#endif
int adreno_vpc_test_entry(void);
#define main adreno_vpc_test_entry
#include "../AdrenoPM4/test_vpc.c"
#undef main
#include <unistd.h>
int main(void) {
    if(getpid()!=1 || getpagesize()!=4096 || sysconf(_SC_PAGESIZE)!=4096) _exit(61);
    if(adreno_vpc_test_entry() || positives!=16597 || refusals!=36) _exit(94);
    _exit(93);
}
