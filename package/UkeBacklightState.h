/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

struct UkeBacklightState {
    uint32_t requested;
    bool on;
};
static inline bool ukeBacklightInitialReady(int32_t first, int32_t second) {
    return first >= 0 && first <= 2047 && second >= 0 && second <= 2047;
}
static inline uint32_t ukeBacklightLevelForNits(int32_t fixed) {
    if (fixed <= 0) return 0;
    const uint64_t scale = 500ull << 16;
    const uint64_t value = ((uint64_t)fixed * 2047 + scale / 2) / scale;
    return value < 1 ? 1 : value > 2047 ? 2047 : (uint32_t)value;
}
static inline uint32_t ukeBacklightEffective(const UkeBacklightState &state) {
    return state.on ? state.requested : 0;
}
static inline void ukeBacklightRequest(UkeBacklightState &state, uint32_t level) {
    /* Screen-off requests may be a nonzero CoreBrightness floor (8 on 24A446).
     * Only lit requests update the brightness to restore on wake. */
    if (state.on && level) state.requested = level > 2047 ? 2047 : level;
}
static inline uint32_t ukeBacklightPower(UkeBacklightState &state, bool on) {
    state.on = on;
    return ukeBacklightEffective(state);
}
/* 24A446 IOMobileGraphicsFamily selector 12: one scalar, no structure;
 * its wrapper loads uint32 scalar[0] and calls request_power_change. */
static inline bool ukeBacklightDecodePower(const uint64_t *input, uint32_t count,
                                           uint64_t structureBytes, bool &on) {
    if (!input || count != 1 || structureBytes || input[0] > 1) return false;
    on = input[0] != 0;
    return true;
}
