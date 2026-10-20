// SPDX-License-Identifier: MIT
// Reference run of the recorded Turnip submission (build/turnip-smoke/TurnipReplay.h) under the stock Linux msm driver, to tell whether the IB
// itself stops the CP or only our Darwin driver does. Static, libc-free aarch64 Linux program for the initramfs built by build-linux-ref-boot.py.
// Linux puts per-process GPU addresses at 0x100000000 and up, so every buffer is placed at its recorded address plus kShift and every 64-bit
// address in the recording (low word inside a recorded buffer, high word 0) is shifted the same way; the patch count is printed per buffer.
// Each run uses a new DRM file (a new context and address space) and freshly filled buffers, like the int-066 scan.
#include <stddef.h>
#include <stdint.h>
#include "TurnipReplay.h"

namespace {
long sys(long n, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0) {
    register long x8 asm("x8") = n, x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x3 asm("x3") = d, x4 asm("x4") = e, x5 asm("x5") = f;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
enum : long {SysOpenat = 56, SysClose = 57, SysWrite = 64, SysIoctl = 29, SysMmap = 222, SysMunmap = 215, SysExit = 94, SysClock = 113};

char out[256];
unsigned outLen;
void flush() {sys(SysWrite, 1, (long)out, outLen); outLen = 0;}
void put(const char *s) {while (*s) {if (outLen == sizeof(out)) flush(); out[outLen++] = *s++;}}
void hex(uint64_t v) {char b[19]; int i = 18; b[i] = 0; do {b[--i] = "0123456789abcdef"[v & 15]; v >>= 4;} while (v); b[--i] = 'x'; b[--i] = '0'; put(b + i);}
void dec(int64_t v) {char b[24]; int i = 23; b[i] = 0; bool neg = v < 0; uint64_t u = neg ? -v : v; do {b[--i] = '0' + u % 10; u /= 10;} while (u); if (neg) b[--i] = '-'; put(b + i);}
void line() {put("\n"); flush();}

uint64_t nowNs() {int64_t ts[2]; sys(SysClock, 1, (long)ts); return ts[0] * 1000000000ull + ts[1];}

constexpr unsigned long ioc(unsigned dir, unsigned nr, unsigned size) {return ((unsigned long)dir << 30) | ((unsigned long)size << 16) | (0x64u << 8) | (0x40u + nr);}
struct Param {uint32_t pipe, param; uint64_t value; uint32_t len, pad;};
struct GemNew {uint64_t size; uint32_t flags, handle;};
struct GemInfo {uint32_t handle, info; uint64_t value; uint32_t len, pad;};
struct SubmitCmd {uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs; uint64_t relocs;};
struct SubmitBo {uint32_t flags, handle; uint64_t presumed;};
struct Submit {uint32_t flags, fence, nr_bos, nr_cmds; uint64_t bos, cmds; int32_t fence_fd; uint32_t queueid; uint64_t in_syncobjs, out_syncobjs;
               uint32_t nr_in, nr_out, stride, pad;};
struct WaitFence {uint32_t fence, flags; int64_t sec, nsec; uint32_t queueid, pad;};
struct GemClose {uint32_t handle, pad;};
const unsigned long IocGetParam = ioc(3, 0, sizeof(Param)), IocGemNew = ioc(3, 2, sizeof(GemNew)), IocGemInfo = ioc(3, 3, sizeof(GemInfo)),
                    IocSubmit = ioc(3, 6, sizeof(Submit)), IocWait = ioc(1, 7, sizeof(WaitFence));
const unsigned long IocGemClose = (1ul << 30) | (sizeof(GemClose) << 16) | (0x64u << 8) | 0x09;   // DRM_IOCTL_GEM_CLOSE
enum : uint32_t {PipeGpu = 0x10, ParamChipId = 3, ParamFaults = 9, ParamVaStart = 0xe, BoWc = 0x20000, InfoOffset = 0, InfoSetIova = 4,
                 CmdBuf = 1, BoRead = 1, BoWrite = 2};
constexpr uint64_t kShift = 0x100000000ull;
constexpr unsigned kCount = sizeof(turnipreplay::kBuffers) / sizeof(turnipreplay::kBuffers[0]);
constexpr unsigned kCuts = sizeof(turnipreplay::kCuts) / sizeof(turnipreplay::kCuts[0]);

int openGpu() {return (int)sys(SysOpenat, -100, (long)"/dev/dri/renderD128", 2 /* O_RDWR */);}
uint64_t param(int fd, uint32_t which) {Param p{PipeGpu, which, 0, 0, 0}; return sys(SysIoctl, fd, IocGetParam, (long)&p) ? ~0ull : p.value;}

bool recorded(uint32_t lo) {
    for (const auto &b : turnipreplay::kBuffers) if (lo >= b.iova && lo < b.iova + b.bytes) return true;
    return false;
}
// Copies a recorded buffer and shifts each 64-bit address pair that points into a recorded buffer. Returns the number of shifted pairs.
unsigned load(volatile uint32_t *to, const turnipreplay::Buffer &b) {
    unsigned shifted = 0;
    for (uint64_t i = 0; i < b.bytes / 4; ++i) to[i] = i < b.words ? b.data[i] : 0;
    for (uint32_t i = 0; i + 1 < b.words; ++i) {
        if (!recorded(b.data[i])) continue;
        if (b.data[i + 1] == 0) {to[i + 1] = uint32_t(kShift >> 32); ++shifted; ++i;}
        // Draw-state register lists hold (register, value) pairs: a 64-bit register is "R lo R+1 hi" (SP_PS_BASE in this recording).
        else if (i && i + 2 < b.words && b.data[i + 1] == b.data[i - 1] + 1 && b.data[i + 2] == 0) {to[i + 2] = uint32_t(kShift >> 32); ++shifted; i += 2;}
    }
    return shifted;
}

struct Run {int status; uint64_t ms; uint64_t faults; uint32_t target[4]; unsigned targetNonzero;};
// One run: allocate, place, fill, submit the first `dwords` of the IB, wait up to 3 s (Linux hangcheck recovers a stuck CP itself), free.
// shared >= 0 reuses that DRM file (one context and page table, so the next run maps new pages at the addresses this run unmapped, as
// our Darwin driver does); otherwise a new file gives a new context.
Run runOnce(uint32_t dwords, bool report, int shared = -1) {
    Run r{};
    const int fd = shared >= 0 ? shared : openGpu();
    if (fd < 0) {r.status = -1000 + fd; return r;}
    uint32_t handles[kCount] = {};
    volatile uint32_t *maps[kCount] = {};
    SubmitBo bos[kCount] = {};
    unsigned ib = 0;
    for (unsigned i = 0; i < kCount; ++i) {
        const auto &b = turnipreplay::kBuffers[i];
        GemNew n{b.bytes, BoWc, 0};
        if (sys(SysIoctl, fd, IocGemNew, (long)&n)) {r.status = -2000 - i; goto done;}
        handles[i] = n.handle;
        GemInfo set{n.handle, InfoSetIova, b.iova + kShift, 0, 0};
        const long s = sys(SysIoctl, fd, IocGemInfo, (long)&set);
        if (s) {r.status = -3000 - i; if (report) {put("  set_iova "); dec(s); line();} goto done;}
        GemInfo off{n.handle, InfoOffset, 0, 0, 0};
        if (sys(SysIoctl, fd, IocGemInfo, (long)&off)) {r.status = -4000 - i; goto done;}
        const long m = sys(SysMmap, 0, b.bytes, 3, 1 /* MAP_SHARED */, fd, off.value);
        if (m < 0 && m > -4096) {r.status = -5000 - i; goto done;}
        maps[i] = (volatile uint32_t *)m;
        const unsigned shifted = load(maps[i], b);
        if (report) {put("  buf "); dec(i); put(" "); hex(b.iova + kShift); put(" +"); hex(b.bytes); put(" shifted "); dec(shifted); line();}
        bos[i] = {BoRead | BoWrite, n.handle, b.iova + kShift};
        if (b.iova == turnipreplay::kIbIova) ib = i;
    }
    {
        const uint64_t faults0 = param(fd, ParamFaults);
        SubmitCmd cmd{CmdBuf, ib, 0, dwords * 4, 0, 0, 0};
        Submit s{PipeGpu, 0, kCount, 1, (uint64_t)bos, (uint64_t)&cmd, -1, 0, 0, 0, 0, 0, 0, 0};
        const uint64_t t0 = nowNs();
        const long rs = sys(SysIoctl, fd, IocSubmit, (long)&s);
        if (rs) {r.status = -6000 + (int)rs; goto done;}
        const uint64_t deadline = t0 + 3000000000ull;
        WaitFence w{s.fence, 0, int64_t(deadline / 1000000000ull), int64_t(deadline % 1000000000ull), 0, 0};
        r.status = (int)sys(SysIoctl, fd, IocWait, (long)&w);
        r.ms = (nowNs() - t0) / 1000000;
        r.faults = param(fd, ParamFaults) - faults0;
        for (unsigned i = 0; i < kCount; ++i)
            if (turnipreplay::kBuffers[i].words == 0 && maps[i]) {
                for (unsigned k = 0; k < 4; ++k) r.target[k] = maps[i][k];
                for (uint64_t k = 0; k < turnipreplay::kBuffers[i].bytes / 4; ++k) r.targetNonzero += maps[i][k] != 0;
            }
    }
done:
    for (unsigned i = 0; i < kCount; ++i) {
        if (maps[i]) sys(SysMunmap, (long)maps[i], turnipreplay::kBuffers[i].bytes);
        if (handles[i]) {GemClose c{handles[i], 0}; sys(SysIoctl, fd, IocGemClose, (long)&c);}
    }
    if (shared < 0) sys(SysClose, fd);
    return r;
}
void show(const char *what, uint32_t dwords, const Run &r) {
    put(what); put(" dwords "); dec(dwords); put(" status "); dec(r.status); put(" ms "); dec(r.ms); put(" faults "); dec(r.faults);
    put(" target "); for (auto w : r.target) {hex(w); put(" ");} put("nz "); dec(r.targetNonzero); line();
}
}  // namespace

extern "C" [[noreturn]] void start() {
    put("msm-ref: Turnip replay under Linux msm, shift "); hex(kShift); line();
    const int fd = openGpu();
    if (fd < 0) {put("msm-ref: open renderD128 failed "); dec(fd); line(); sys(SysExit, 1);}
    put("chip "); hex(param(fd, ParamChipId)); put(" va_start "); hex(param(fd, ParamVaStart)); put(" faults "); dec(param(fd, ParamFaults)); line();
    sys(SysClose, fd);
    const uint32_t whole = turnipreplay::kCuts[kCuts - 1];
    Run r = runOnce(whole, true);
    show("WHOLE", whole, r);
    // A second whole run in a new context: does the first run change anything for the next one (int-063 vs int-065 on our driver)?
    show("WHOLE2", whole, runOnce(whole, false));
    if (r.status) for (unsigned c = 0; c < kCuts; ++c) {
        Run p = runOnce(turnipreplay::kCuts[c], false);
        put("op "); hex(turnipreplay::kCutOps[c]); put(" "); show("CUT", turnipreplay::kCuts[c], p);
        if (p.status) break;
    }
    // Remap in one page table: five whole runs in one DRM file, each on new pages at the same GPU addresses.
    const int same = openGpu();
    for (unsigned k = 0; k < 5; ++k) {
        Run s = runOnce(whole, false, same);
        put("SAMEFD "); dec(k); put(" "); show("WHOLE", whole, s);
        if (s.status) break;
    }
    sys(SysClose, same);
    put("msm-ref: done"); line();
    sys(SysExit, 0);
    __builtin_unreachable();
}
