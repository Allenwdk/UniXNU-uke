/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4K.h"
#include <kern/cpu_data.h>
#include <arm/pmap.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <string.h>
#if DEVELOPMENT || DEBUG
#define POOL_PAGES 6
#define DATA_PAGES 2
#define SENTINEL UINT64_C(0xa5a5a5a5a5a5a5a5)
volatile int native_smmu4k_status = -1;
volatile uint32_t native_smmu4k_table_pages;
volatile uint32_t native_smmu4k_leaf_pages;
volatile uint32_t native_smmu4k_refusal_mask;
volatile uint32_t native_smmu4k_output_preserved;
volatile uint32_t native_smmu4k_allocations_released;
volatile uint64_t native_smmu4k_pool_pointer;
volatile uint64_t native_smmu4k_data_pointer;
volatile uint64_t native_smmu4k_cpu_mair;
uint64_t native_smmu4k_pool_physical[POOL_PAGES];
uint64_t native_smmu4k_data_physical[DATA_PAGES];

__attribute__((noinline)) void native_smmu4k_checkpoint(void)
{
    __asm__ volatile("" ::: "memory");
}
static bool untouched(const uint64_t *pool)
{
    for (size_t i = 0; i < POOL_PAGES * 512; ++i) if (pool[i] != SENTINEL) return false;
    return true;
}
static bool zero_result(const ArmSmmu4KResult *result)
{
    return !result->root_physical && !result->mair && !result->table_pages &&
        !result->leaf_pages && !result->start_level;
}
void native_smmu4k_probe(void)
{
    int enabled = 0;
    if (!PE_parse_boot_argn("offline-smmu4k", &enabled, sizeof(enabled)) || !enabled) return;
    vm_offset_t pool_va = 0, data_va = 0;
    native_smmu4k_status = 1;
    if (PAGE_SIZE != 4096 || page_size != 4096 || VM_MAP_PAGE_SIZE(kernel_map) != 4096) goto done;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(native_smmu4k_cpu_mair));
    /* The real allocator is bounded by KMA_NOPAGEWAIT; no physical addresses
     * are invented. No allocation is published as an active SMMU table. */
    if (kmem_alloc_contig(kernel_map, &pool_va, POOL_PAGES * 4096, 4095, 0, 0,
        KMA_NOPAGEWAIT | KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG)) goto done;
    if (kmem_alloc_contig(kernel_map, &data_va, DATA_PAGES * 4096, 4095, 0, 0,
        KMA_NOPAGEWAIT | KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG)) goto done;
    native_smmu4k_pool_pointer = pool_va;
    native_smmu4k_data_pointer = data_va;
    native_smmu4k_status = 2;
    for (unsigned i = 0; i < POOL_PAGES; ++i) {
        native_smmu4k_pool_physical[i] = kvtophys(pool_va + i * 4096);
        if (!native_smmu4k_pool_physical[i] || (native_smmu4k_pool_physical[i] & 4095) ||
            native_smmu4k_pool_physical[i] != native_smmu4k_pool_physical[0] + i * 4096) goto done;
    }
    for (unsigned i = 0; i < DATA_PAGES; ++i) {
        native_smmu4k_data_physical[i] = kvtophys(data_va + i * 4096);
        if (!native_smmu4k_data_physical[i] || (native_smmu4k_data_physical[i] & 4095) ||
            native_smmu4k_data_physical[i] != native_smmu4k_data_physical[0] + i * 4096) goto done;
    }
    uint64_t *pool = (uint64_t *)pool_va;
    for (size_t i = 0; i < POOL_PAGES * 512; ++i) pool[i] = SENTINEL;
    memset((void *)data_va, 0x39, DATA_PAGES * 4096);
    ArmSmmu4KConfig config = {48, 48, native_smmu4k_pool_physical[0], 16};
    ArmSmmu4KRange range = {0x1ff000, native_smmu4k_data_physical[0], 8192, 3, 1, 2};
    ArmSmmu4KResult result;
    native_smmu4k_output_preserved = 1;
    native_smmu4k_status = 3;
    for (unsigned control = 0; control < 6; ++control) {
        ArmSmmu4KRange bad = range;
        ArmSmmu4KConfig cfg = config;
        ArmSmmu4KStatus expected;
        size_t capacity = POOL_PAGES;
        if (control == 0) { capacity = 4; expected = ArmSmmu4KCapacity; }
        else if (control == 1) { bad.physical = config.table_physical; expected = ArmSmmu4KOverlap; }
        else if (control == 2) { ++bad.iova; expected = ArmSmmu4KBadRange; }
        else if (control == 3) { cfg.leaf_budget = 1; expected = ArmSmmu4KBudget; }
        else if (control == 4) { bad.permissions = 2; expected = ArmSmmu4KBadRange; }
        else { bad.physical = UINT64_C(1) << 48; expected = ArmSmmu4KBadRange; }
        memset(&result, 0xa5, sizeof(result));
        ArmSmmu4KStatus status = ArmSmmu4KBuild(&cfg, &bad, 1, pool, capacity, &result);
        if (status == expected && zero_result(&result)) native_smmu4k_refusal_mask |= 1u << control;
        if (!untouched(pool)) native_smmu4k_output_preserved = 0;
    }
    native_smmu4k_status = 4;
    if (ArmSmmu4KBuild(&config, &range, 1, pool, POOL_PAGES, &result) ||
        result.table_pages != 5 || result.leaf_pages != 2 || result.start_level ||
        result.root_physical != config.table_physical || result.mair != 0xff44) goto done;
    native_smmu4k_table_pages = (uint32_t)result.table_pages;
    native_smmu4k_leaf_pages = (uint32_t)result.leaf_pages;
    /* Fixed fixture descriptor checks, never a deployment software walker. */
    for (unsigned i = 0; i < 4; ++i) {
        size_t slot = i == 3 ? 2 * 512 + 1 : i * 512;
        if (pool[slot] != ((config.table_physical + (i + 1) * 4096) | 3)) goto done;
    }
    const uint64_t flags = (UINT64_C(3) << 53) | 0xe47;
    if (pool[3 * 512 + 511] != (range.physical | flags) ||
        pool[4 * 512] != ((range.physical + 4096) | flags)) goto done;
    for (size_t i = 5 * 512; i < POOL_PAGES * 512; ++i) if (pool[i] != SENTINEL) goto done;
    native_smmu4k_status = native_smmu4k_refusal_mask == 63 && native_smmu4k_output_preserved ? 0 : 5;
done:
    native_smmu4k_checkpoint();
    /* Retain allocations only while the real checkpoint is stopped. */
    if (data_va) kmem_free(kernel_map, data_va, DATA_PAGES * 4096);
    if (pool_va) kmem_free(kernel_map, pool_va, POOL_PAGES * 4096);
    native_smmu4k_pool_pointer = native_smmu4k_data_pointer = 0;
    native_smmu4k_allocations_released = 1;
}
#endif
