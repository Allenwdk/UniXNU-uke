/* SPDX-License-Identifier: MIT */
#include "AdrenoPM4.h"
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#if DEVELOPMENT || DEBUG
volatile int native_adreno_pm4_status = -1;
volatile uint32_t native_adreno_pm4_packets;
volatile uint32_t native_adreno_pm4_refusal_mask;
volatile uint32_t native_adreno_pm4_output_preserved;
volatile uint32_t native_adreno_pm4_bit_refusals;
volatile uint32_t native_adreno_pm4_input_preserved;
__attribute__((noinline)) void native_adreno_pm4_checkpoint(void)
{
    __asm__ volatile("" ::: "memory");
}
static bool refuses(const uint32_t *words, size_t length, size_t capacity, AdrenoPM4Status expected)
{
    AdrenoPM4Packet output[4], before[4];
    size_t count = 123;
    memset(output, 0xa5, sizeof(output));
    memcpy(before, output, sizeof(output));
    AdrenoPM4Status status = AdrenoPM4Plan(words, length, output, capacity, &count);
    if (memcmp(output, before, sizeof(output))) native_adreno_pm4_output_preserved = 0;
    return status == expected && count == 0;
}
void native_adreno_pm4_probe(void)
{
    int enabled = 0;
    if (!PE_parse_boot_argn("offline-adreno-pm4", &enabled, sizeof(enabled)) || !enabled) return;
    uint32_t words[5] = {0, 0xffffffff, 0, 0x70000000, 0};
    uint32_t before[5];
    AdrenoPM4Packet packets[3];
    size_t count;
    native_adreno_pm4_status = 1;
    if (AdrenoPM4Type7(0x10, 1, words) || words[0] != 0x70100001 ||
        AdrenoPM4Type4(0, 1, words + 2) || words[2] != 0x48000001 ||
        AdrenoPM4Type7(0x26, 0, words + 4) || words[4] != 0x70268000) goto done;
    memcpy(before, words, sizeof(words));
    native_adreno_pm4_status = 2;
    if (AdrenoPM4Plan(words, 5, packets, 3, &count) || count != 3 ||
        packets[0].header_word != 0 || packets[0].identifier != 0x10 || packets[0].payload_words != 1 ||
        packets[1].header_word != 2 || packets[1].type != 4 || packets[1].payload_words != 1 ||
        packets[2].header_word != 4 || packets[2].identifier != 0x26 || packets[2].payload_words != 0) goto done;
    native_adreno_pm4_packets = (uint32_t)count;
    native_adreno_pm4_output_preserved = 1;
    if (refuses(words, 3, 3, AdrenoPM4Truncated)) native_adreno_pm4_refusal_mask |= 1;
    if (refuses(words, 5, 2, AdrenoPM4Capacity)) native_adreno_pm4_refusal_mask |= 2;
    uint32_t bad[5];
    memcpy(bad, words, sizeof(bad));
    bad[4] ^= 1u << 15;
    if (refuses(bad, 5, 3, AdrenoPM4InvalidHeader)) native_adreno_pm4_refusal_mask |= 4;
    bad[0] = 0xc0001000;
    if (refuses(bad, 1, 3, AdrenoPM4InvalidHeader)) native_adreno_pm4_refusal_mask |= 8;
    uint32_t header = 0xa5a5a5a5;
    if (AdrenoPM4Type7(128, 1, &header) == AdrenoPM4InvalidArgument && header == 0xa5a5a5a5)
        native_adreno_pm4_refusal_mask |= 16;
    if (AdrenoPM4Type4(0x3ffff, 2, &header) == AdrenoPM4InvalidArgument && header == 0xa5a5a5a5)
        native_adreno_pm4_refusal_mask |= 32;
    for (unsigned type = 0; type < 2; ++type) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            memcpy(bad, words, sizeof(bad));
            bad[type * 2] ^= 1u << bit;
            if (refuses(bad, 5, 3, AdrenoPM4InvalidHeader)) ++native_adreno_pm4_bit_refusals;
        }
    }
    native_adreno_pm4_input_preserved = !memcmp(before, words, sizeof(words));
    native_adreno_pm4_status = native_adreno_pm4_refusal_mask == 63 && native_adreno_pm4_bit_refusals == 64 &&
        native_adreno_pm4_output_preserved && native_adreno_pm4_input_preserved ? 0 : 3;
done:
    native_adreno_pm4_checkpoint();
}
#endif
