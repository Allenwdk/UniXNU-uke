/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Stores into the mapped WPSS firmware reservation. Ordinary memcpy/bzero
 * may issue unaligned wide stores or DC ZVA, which this mapping rejects.
 * Bulk accesses are aligned 32-bit stores; byte heads/tails touch only the
 * requested range. The loader retains its completion barrier before PAS. */
#ifndef UKE_WPSS_MEMORY_H
#define UKE_WPSS_MEMORY_H
#include <stddef.h>
#include <stdint.h>

static inline void uke_wpss_memory_copy(volatile uint8_t *to, const uint8_t *from, size_t bytes)
{
    while (bytes && ((uintptr_t)to & 3)) {
        *to++ = *from++;
        --bytes;
    }
    while (bytes >= 4) {
        const uint32_t value = (uint32_t)from[0] | (uint32_t)from[1] << 8 |
                               (uint32_t)from[2] << 16 | (uint32_t)from[3] << 24;
        *(volatile uint32_t *)to = value;
        to += 4;
        from += 4;
        bytes -= 4;
    }
    while (bytes) {
        *to++ = *from++;
        --bytes;
    }
}

static inline void uke_wpss_memory_zero(volatile uint8_t *to, size_t bytes)
{
    while (bytes && ((uintptr_t)to & 3)) {
        *to++ = 0;
        --bytes;
    }
    while (bytes >= 4) {
        *(volatile uint32_t *)to = 0;
        to += 4;
        bytes -= 4;
    }
    while (bytes) {
        *to++ = 0;
        --bytes;
    }
}
#endif
