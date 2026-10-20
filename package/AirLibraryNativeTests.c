/* SPDX-License-Identifier: MIT */
/* Execute the unchanged generic unit suite through actual native iOS17 libc. */
#ifdef NDEBUG
#error Native library controls require active assertions
#endif
#define main air_library_test_entry
#include "../AirNir/tests/test_library.c"
#undef main
int main(void) {
    if (getpid()!=1 || getpagesize()!=4096 || sysconf(_SC_PAGESIZE)!=4096) _exit(61);
    char *argv[]={"native-library-tests",
        "/System/Library/Frameworks/QuartzCore.framework/default.metallib",
        "/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib",0};
    if (air_library_test_entry(3,argv) || positive!=12 || negative!=4456) _exit(91);
    _exit(90);
}
