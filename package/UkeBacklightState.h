/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

static inline bool ukeBacklightInitialReady(int32_t first, int32_t second) {
    return first >= 0 && first <= 2047 && second >= 0 && second <= 2047;
}
static inline uint32_t ukeBacklightLevelForNits(int32_t fixed) {
    if (fixed <= 0) return 0;
    const uint64_t scale = 500ull << 16;
    const uint64_t value = ((uint64_t)fixed * 2047 + scale / 2) / scale;
    return value < 1 ? 1 : value > 2047 ? 2047 : (uint32_t)value;
}
