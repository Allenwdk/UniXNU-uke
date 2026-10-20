/* SPDX-License-Identifier: MIT */
#include "ConstantUploads.h"
#include <kern/cpu_data.h>
#include <arm/pmap.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <string.h>
#if DEVELOPMENT || DEBUG
volatile int native_constants_status = -1;
volatile uint32_t native_constants_refusal_mask;
volatile uint32_t native_constants_output_preserved;
volatile uint32_t native_constants_written_bytes;
volatile uint32_t native_constants_padded_bytes;
volatile uint32_t native_constants_allocations_released;
volatile uint64_t native_constants_a_pointer;
volatile uint64_t native_constants_b_pointer;
volatile uint64_t native_constants_output_pointer;
uint64_t native_constants_physical[6];

__attribute__((noinline)) void native_constants_checkpoint(void)
{
    __asm__ volatile("" ::: "memory");
}
static int sentinel(const uint8_t *dst)
{
    for (unsigned i = 0; i < 4096; ++i) if (dst[i] != 0x6b) return 0;
    return 1;
}
void native_constants_probe(void)
{
    int enabled = 0;
    if (!PE_parse_boot_argn("offline-constants", &enabled, sizeof(enabled)) || !enabled) return;
    vm_offset_t allocations[3] = {0};
    native_constants_status = 1;
    if (PAGE_SIZE != 4096 || page_size != 4096 || VM_MAP_PAGE_SIZE(kernel_map) != 4096) goto done;
    for (unsigned i = 0; i < 3; ++i) {
        /* Size includes a real final guard page, never a logical mapped tail. */
        if (kernel_memory_allocate(kernel_map, &allocations[i], 8192, 4095,
            KMA_NOPAGEWAIT | KMA_KOBJECT | KMA_DATA | KMA_GUARD_LAST,
            VM_KERN_MEMORY_DIAG)) goto done;
        native_constants_physical[2*i] = kvtophys(allocations[i]);
        native_constants_physical[2*i+1] = kvtophys(allocations[i] + 4096);
        if (!native_constants_physical[2*i] || native_constants_physical[2*i+1]) goto done;
    }
    native_constants_a_pointer = allocations[0];
    native_constants_b_pointer = allocations[1];
    native_constants_output_pointer = allocations[2];
    uint8_t *a = (uint8_t *)(allocations[0] + 4096 - 8);
    uint8_t *b = (uint8_t *)(allocations[1] + 4096 - 80);
    uint8_t *page = (uint8_t *)allocations[2];
    uint8_t *dst = page + 4096 - 256;
    memset((void *)allocations[0], 0x39, 4096);
    memset((void *)allocations[1], 0x49, 4096);
    for (unsigned i = 0; i < 8; ++i) a[i] = (uint8_t)(i + 1);
    for (unsigned i = 0; i < 80; ++i) b[i] = (uint8_t)(i + 17);
    struct air_constant_binding bindings[16] = {0};
    bindings[3] = (struct air_constant_binding){a, 8, 8};
    bindings[7] = (struct air_constant_binding){b, 80, 80};
    struct air_constant_range ranges[2] = {{3, 0, 64, 64}, {7, 64, 128, 128}};
    struct air_constant_read reads[2] = {{3, 0, 8}, {7, 64, 16}};
    uint32_t immediates[4] = {0x11223344, 0x89abcdef, 0, 0xffffffff};
    struct air_constant_layout layout = {256, 64, 64, 128, ranges, 2, reads, 2, 208, 4, immediates};
    native_constants_output_preserved = 1;
    native_constants_status = 2;
    for (unsigned i = 0; i < 10; ++i) {
        struct air_constant_layout bad = layout;
        struct air_constant_binding saved = bindings[3];
        struct air_constant_read read_saved = reads[0];
        struct air_constant_range range_saved = ranges[1];
        size_t capacity = 256;
        uint32_t expected;
        if (i == 0) { capacity = 255; expected = AIR_CONSTANT_CAPACITY; }
        else if (i == 1) { bad.upload_alignment = 48; expected = AIR_CONSTANT_LAYOUT; }
        else if (i == 2) { bindings[3].bytes = 7; expected = AIR_CONSTANT_BINDING; }
        else if (i == 3) { bindings[3].data = NULL; expected = AIR_CONSTANT_BINDING; }
        else if (i == 4) { reads[0].bytes = 12; expected = AIR_CONSTANT_READ; }
        else if (i == 5) { ranges[1].destination_begin = 64; expected = AIR_CONSTANT_LAYOUT; }
        else if (i == 6) { bindings[3].data = dst; expected = AIR_CONSTANT_ALIAS; }
        else if (i == 7) { bad.immediates = (const uint32_t *)dst; expected = AIR_CONSTANT_ALIAS; }
        else if (i == 8) { bad.ranges = (const struct air_constant_range *)dst; expected = AIR_CONSTANT_ALIAS; }
        else { bad.immediate_begin = 64; expected = AIR_CONSTANT_LAYOUT; }
        memset(page, 0x6b, 4096);
        struct air_constant_result r = air_pack_constants(&bad, bindings, dst, capacity);
        if (r.status == expected && !r.written_bytes && !r.padded_bytes)
            native_constants_refusal_mask |= 1u << i;
        if (!sentinel(page)) native_constants_output_preserved = 0;
        bindings[3] = saved;
        reads[0] = read_saved;
        ranges[1] = range_saved;
    }
    native_constants_status = 3;
    struct air_constant_result r = air_pack_constants(&layout, bindings, dst, 256);
    native_constants_written_bytes = r.written_bytes;
    native_constants_padded_bytes = r.padded_bytes;
    if (r.status || r.written_bytes != 144 || r.padded_bytes != 104) goto done;
    for (unsigned i = 0; i < 4096; ++i) {
        uint8_t expected = 0x6b;
        if (i >= 3840 + 64 && i < 3840 + 128) expected = i < 3840 + 72 ? a[i - 3840 - 64] : 0;
        if (i >= 3840 + 128 && i < 3840 + 192) expected = i < 3840 + 144 ? b[i - 3840 - 64] : 0;
        if (i >= 3840 + 208 && i < 3840 + 224) {
            unsigned offset = i - 3840 - 208;
            expected = (uint8_t)(immediates[offset / 4] >> ((offset % 4) * 8));
        }
        if (page[i] != expected) goto done;
    }
    native_constants_status = native_constants_refusal_mask == 1023 && native_constants_output_preserved ? 0 : 4;
done:
    native_constants_checkpoint();
    for (unsigned i = 0; i < 3; ++i) if (allocations[i]) kmem_free(kernel_map, allocations[i], 8192);
    native_constants_a_pointer = native_constants_b_pointer = native_constants_output_pointer = 0;
    native_constants_allocations_released = 1;
}
#endif
