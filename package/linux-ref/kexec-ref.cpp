// SPDX-License-Identifier: MIT
// Restarts a running reference session into a kernel with a new command line, without the bootloader: kexec_file_load of /run/Image (an
// uncompressed arm64 Image), /run/initrd.gz and the text in /run/cmdline, then reboot(LINUX_REBOOT_CMD_KEXEC). The kernel reuses the running
// device tree with the new command line. Static, libc-free aarch64 Linux program, built like msm-ref.
namespace {
long sys(long n, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0) {
    register long x8 asm("x8") = n, x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x3 asm("x3") = d, x4 asm("x4") = e;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}
enum : long {SysOpenat = 56, SysRead = 63, SysWrite = 64, SysExit = 94, SysReboot = 142, SysSync = 81, SysKexecFile = 294};
void say(const char *s, long v) {
    char b[96]; int n = 0;
    while (*s && n < 60) b[n++] = *s++;
    b[n++] = ' ';
    if (v < 0) {b[n++] = '-'; v = -v;}
    char d[20]; int k = 0; do {d[k++] = char('0' + v % 10); v /= 10;} while (v);
    while (k) b[n++] = d[--k];
    b[n++] = '\n';
    sys(SysWrite, 1, (long)b, n);
}
char cmdline[2048];
}  // namespace

extern "C" [[noreturn]] void start() {
    const long kernel = sys(SysOpenat, -100, (long)"/run/Image", 0), initrd = sys(SysOpenat, -100, (long)"/run/initrd.gz", 0);
    const long cmd = sys(SysOpenat, -100, (long)"/run/cmdline", 0);
    say("open kernel/initrd/cmdline", kernel < 0 ? kernel : initrd < 0 ? initrd : cmd);
    if (kernel < 0 || initrd < 0 || cmd < 0) sys(SysExit, 1);
    long len = sys(SysRead, cmd, (long)cmdline, sizeof(cmdline) - 1);
    if (len < 0) len = 0;
    while (len && (cmdline[len - 1] == '\n' || cmdline[len - 1] == ' ')) --len;
    cmdline[len] = 0;
    const long loaded = sys(SysKexecFile, kernel, initrd, len + 1, (long)cmdline, 0);
    say("kexec_file_load", loaded);
    if (loaded) sys(SysExit, 2);
    sys(SysSync);
    say("reboot kexec", sys(SysReboot, 0xfee1dead, 672274793, 0x45584543, 0));
    sys(SysExit, 3);
    __builtin_unreachable();
}
