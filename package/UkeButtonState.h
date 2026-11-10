/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

/* Report 2 stays identical to ShengHidInput. Failed reads preserve each group;
 * only GPIO6 needs the stock DTS's 32 ms debounce. Used by the real worker. */
struct UkeButtonState {
    uint32_t mask, gpioCandidate;
    uint64_t gpioSince;
    bool gpioKnown;
};
static inline bool ukeButtonOwned(uint32_t map, uint32_t owner, uint32_t ppid, uint32_t ee) {
    return map && ((map >> 8) & 0xfff) == ppid && (map & (1u << 24)) && (owner & 7u) == ee;
}
static inline uint32_t ukeButtonSample(UkeButtonState &s, bool ponOK, uint8_t pon,
                                       bool gpioOK, uint8_t gpio, uint64_t ms) {
    if (ponOK) s.mask = (s.mask & ~10u) | ((pon & 0x80) ? 2u : 0u) | ((pon & 0x40) ? 8u : 0u);
    if (gpioOK) {
        const uint32_t value = (gpio & 1u) ? 0u : 4u;
        if (!s.gpioKnown || value != s.gpioCandidate) {
            s.gpioKnown = true; s.gpioCandidate = value; s.gpioSince = ms;
        } else if (ms - s.gpioSince >= 32) s.mask = (s.mask & ~4u) | value;
    } else s.gpioKnown = false;
    return s.mask;
}
static inline bool ukeButtonDebouncing(const UkeButtonState &s) {
    return s.gpioKnown && (s.mask & 4u) != s.gpioCandidate;
}
