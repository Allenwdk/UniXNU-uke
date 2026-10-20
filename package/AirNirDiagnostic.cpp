// SPDX-License-Identifier: MIT
// Offline instrument only: execute the actual compiler, never rasterize pixels.
#define main air_nir_diagnostic_compile
#include "../AirNir/Frontend.cpp"
#undef main
#include <CommonCrypto/CommonDigest.h>
#include <unistd.h>
#include <cstdio>
#include "AirNirDiagnosticInputs.h"

static bool exact_digest(const char *path, const char *expected) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    CC_SHA256_CTX ctx;
    if (CC_SHA256_Init(&ctx) != 1) { fclose(f); return false; }
    unsigned char bytes[4096], digest[CC_SHA256_DIGEST_LENGTH];
    size_t total = 0, count;
    while ((count = fread(bytes, 1, sizeof(bytes), f))) {
        total += count;
        if (total > 16 * 1024 * 1024 || CC_SHA256_Update(&ctx, bytes, (CC_LONG)count) != 1) {
            fclose(f); return false;
        }
    }
    bool ok = !ferror(f) && total && CC_SHA256_Final(digest, &ctx) == 1;
    fclose(f);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(digest); ++i)
        ok = ok && expected[2*i] == hex[digest[i] >> 4] && expected[2*i+1] == hex[digest[i] & 15];
    return ok;
}
int main() {
    if (getpid() != 1 || getpagesize() != 4096 || sysconf(_SC_PAGESIZE) != 4096) _exit(61);
    if (setenv("IR3_SHADER_DEBUG", "asmroundtrip", 1)) _exit(63);
    for (const auto &test : diagnostic_inputs) {
        char input[256], prefix[256], output[256];
        snprintf(input, sizeof(input), "/private/var/tmp/%s.bc", test.name);
        snprintf(prefix, sizeof(prefix), "/private/var/tmp/%s", test.name);
        if (!exact_digest(input, test.input)) _exit(62);
        char *args[] = {const_cast<char *>("air-native"), input,
            const_cast<char *>("0xffff43050a01"), prefix,
            const_cast<char *>("--compile"), nullptr};
        int result = air_nir_diagnostic_compile(5, args);
        if (result) _exit(64);
        snprintf(output, sizeof(output), "%s.bin", prefix);
        if (!exact_digest(output, test.binary)) _exit(65);
        snprintf(output, sizeof(output), "%s.ir3.json", prefix);
        if (!exact_digest(output, test.abi)) _exit(66);
        snprintf(output, sizeof(output), "%s.interface.json", prefix);
        if (!exact_digest(output, test.interface)) _exit(67);
    }
    // Observe this real exit syscall before PID1 exit handling. No success path
    // exists until native page ABI, original input and actual outputs and complete interfaces all match.
    _exit(73);
}
