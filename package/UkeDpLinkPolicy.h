// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

namespace UkeDpLink {
constexpr uint8_t kRbr = 0x06, kHbr = 0x0a, kHbr2 = 0x14;

// DPCD 0x202–0x207, with two active lanes.
struct Status {
    uint8_t bytes[6]{};
    bool clockRecovered() const {return (bytes[0] & 0x11) == 0x11;}
    bool equalized() const {return (bytes[0] & 0x77) == 0x77 && (bytes[2] & 1);}
};

struct SignalLevels {
    uint8_t swing = 0, pre = 0;

    // Qualcomm programs a shared setting for both lanes. Use the largest
    // request of each, then cap the sum to the DP v1.x limit.
    static SignalLevels requested(const Status &status) {
        const uint8_t both = status.bytes[4];
        const uint8_t swing0 = both & 3, swing1 = (both >> 4) & 3;
        const uint8_t pre0 = (both >> 2) & 3, pre1 = (both >> 6) & 3;
        const uint8_t swing = swing0 > swing1 ? swing0 : swing1;
        uint8_t pre = pre0 > pre1 ? pre0 : pre1;
        if (swing + pre > 3) pre = 3 - swing;
        return {swing, pre};
    }

    uint8_t laneSet() const {
        return swing | (swing == 3 ? 0x04 : 0) | (pre << 3) | (pre == 3 ? 0x20 : 0);
    }
};

inline unsigned trainingDelayUs(uint8_t auxInterval, bool equalization) {
    const unsigned interval = auxInterval & 0x7f;
    // DP 1.2 uses the advertised 4 ms units, up to the DP-defined maximum.
    if (interval && interval <= 4) return interval * 4000;
    return equalization ? 400 : 100;
}
}
