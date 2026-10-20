// SPDX-License-Identifier: MIT
// Keeps the GPU powered for register reads in a reference session: opens the render node, sets MSM_PARAM_SYSPROF to 2 (perfcounters kept and
// runtime suspend disabled while this file is open) and sleeps until killed. With it running, debugfs dri/*/gpu dumps the live GX registers
// and the SMMU context banks can be read with devmem without touching a clock-gated block. Static, libc-free aarch64 Linux program.
#include <stdint.h>
namespace {
long sys(long n, long a = 0, long b = 0, long c = 0, long d = 0) {
    register long x8 asm("x8") = n, x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x3 asm("x3") = d;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}
struct Param {uint32_t pipe, param; uint64_t value; uint32_t len, pad;};
void say(const char *s, long v) {
    char b[80]; int n = 0;
    while (*s && n < 50) b[n++] = *s++;
    b[n++] = ' ';
    if (v < 0) {b[n++] = '-'; v = -v;}
    char d[20]; int k = 0; do {d[k++] = char('0' + v % 10); v /= 10;} while (v);
    while (k) b[n++] = d[--k];
    b[n++] = '\n';
    sys(64, 1, (long)b, n);
}
}  // namespace

extern "C" [[noreturn]] void start() {
    const long fd = sys(56, -100, (long)"/dev/dri/renderD128", 2);
    say("msm-hold open", fd);
    if (fd < 0) sys(94, 1);
    Param p{0x10, 0x0b /* MSM_PARAM_SYSPROF */, 2, 0, 0};
    say("msm-hold sysprof", sys(29, fd, (1ul << 30) | (sizeof(Param) << 16) | (0x64u << 8) | 0x41 /* SET_PARAM */, (long)&p));
    const int64_t ts[2] = {100000, 0};
    for (;;) sys(101, (long)ts, 0);   // nanosleep
}
