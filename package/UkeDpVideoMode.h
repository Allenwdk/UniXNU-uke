// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include "UkeDpLinkPolicy.h"

namespace UkeDpVideo {
struct Mode {
    unsigned pixel, width, height, hfront, hsync, hback, vfront, vsync, vback;
    bool hlow, vlow;
    constexpr unsigned htotal() const {return width + hfront + hsync + hback;}
    constexpr unsigned vtotal() const {return height + vfront + vsync + vback;}
};

struct TransferUnit {
    unsigned size, valid, delay;
    unsigned upperCount = 0, lowerCount = 0;

    constexpr uint32_t boundary2() const {
        return lowerCount ? (lowerCount << 20) | (upperCount << 16) | ((valid - 1) << 1) | 1u : 0;
    }
};

constexpr unsigned linkClock(uint8_t rate) {return rate * 27000u;}
constexpr unsigned pixelParentClock(uint8_t rate) {
    // QMP exports VCO/4 for HBR2 and VCO/2 for HBR/RBR.
    return linkClock(rate) * 10 / (rate == UkeDpLink::kHbr2 ? 4 : 2);
}

// SM7675 uses two pixels per P0 clock on the DPU route. The OEM encoder
// moves the front porch to the back before halving horizontal timings.
constexpr bool wideBusCompatible(const Mode &mode) {
    return ((mode.pixel | mode.width | mode.hsync | (mode.hback+mode.hfront)) & 1u)==0;
}

constexpr unsigned streamPixelClock(const Mode &mode,bool wideBus) {
    return wideBus?mode.pixel/2:mode.pixel;
}

struct PixelRatio {unsigned m, n;};

// Linux rational_best_approximation, specialized for the DP0 16-bit RCG.
// The pixel/parent ratio is below 1/2, so the denominator bounds both fields.
inline PixelRatio pixelRatio(unsigned pixel, uint8_t rate) {
    const unsigned parent = pixelParentClock(rate);
    if (!pixel || pixel > parent / 2) return {};
    unsigned n = pixel, d = parent;
    PixelRatio older{0, 1}, best{1, 0};
    while (d) {
        const unsigned divisor = d, term = n / d;
        d = n % d;
        n = divisor;
        const PixelRatio next{older.m + term * best.m, older.n + term * best.n};
        if (next.n > 0xffff) {
            const unsigned t = (0xffff - older.n) / best.n;
            if (2 * t > term || (2 * t == term && uint64_t(older.n) * divisor > uint64_t(best.n) * d))
                best = {older.m + t * best.m, older.n + t * best.n};
            break;
        }
        older = best;
        best = next;
    }
    return best;
}

struct StreamRatio {unsigned mvid,nvid;};
constexpr StreamRatio streamRatio(unsigned m,unsigned n,bool wideBus,uint8_t rate) {
    const unsigned scale=n<0x8000?0x8000/n:1;
    return {5*m*scale*(wideBus?2:1),n*scale*(rate==UkeDpLink::kHbr2?2:1)};
}

// RGB 8 bpc on two lanes. Use the exact-ratio branch of Qualcomm's TU
// calculation, where no boundary moderation or extra-symbol padding is needed.
inline bool exactTransferUnit(const Mode &mode, uint8_t rate, TransferUnit &out) {
    const unsigned clock = linkClock(rate), payload = 3 * mode.pixel;
    if (!mode.pixel || payload >= 2 * clock) return false;
    for (unsigned size = 32; size <= 64; ++size) {
        const unsigned symbols = size * payload;
        if (symbols % (2 * clock)) continue;
        const unsigned valid = symbols / (2 * clock);
        if (!valid || ((mode.width * 3) / valid) % 2) continue;
        const unsigned delay = size - valid + (4 * clock + mode.pixel - 1) / mode.pixel;
        constexpr unsigned margin = 4;
        if (mode.hfront + mode.hsync + mode.hback <= margin ||
            uint64_t(mode.htotal() - mode.width - margin) * clock < uint64_t(delay) * mode.pixel) continue;
        out = {size, valid, delay};
        return true;
    }
    return false;
}

constexpr unsigned divideUp(uint64_t value, unsigned divisor) {
    return (value + divisor - 1) / divisor;
}

// Qualcomm dp_panel.c TU selection for synchronous RGB 8 bpc SST on two
// lanes. Keep the legacy boundary and the moderated upper/lower search,
// including the partial line tail and the extra symbols needed at startup.
inline bool transferUnit(const Mode &mode, uint8_t rate, TransferUnit &out) {
    constexpr unsigned lanes = 2, bytesPerPixel = 3, margin = 4, maxDelay = 1023;
    const unsigned clock = linkClock(rate), capacity = lanes * clock, payload = bytesPerPixel * mode.pixel;
    const unsigned blank = mode.hfront + mode.hsync + mode.hback, lineBytes = bytesPerPixel * mode.width;
    if (!payload || payload >= capacity || !lineBytes || mode.width % lanes || blank <= margin) return false;
    if (blank >= 160 && exactTransferUnit(mode, rate, out)) return true;
    const unsigned delayBudget = uint64_t(blank - margin < mode.width ? blank - margin : mode.width) * clock / mode.pixel;
    const unsigned maxStart = delayBudget < maxDelay ? delayBudget : maxDelay;
    const unsigned bufferMargin = divideUp(uint64_t(margin) * clock, mode.pixel);
    const auto fitsLine = [&](unsigned delay, unsigned size, unsigned perLane, unsigned sent) {
        if (delay > maxStart) return false;
        if (blank >= 160) return true;
        // OEM reduced-blanking v2 check: CDC, DP pipe, BE, BS and line tail.
        const unsigned lineClocks = uint64_t(mode.htotal()) * clock / (lanes * mode.pixel);
        const unsigned remaining = lineBytes > sent * lanes ? divideUp(lineBytes - sent * lanes, lanes) : 0;
        const unsigned pipe = divideUp(clock, mode.pixel) + 9 + 5;
        return pipe + delay + perLane * size + remaining + 1 <= lineClocks;
    };

    TransferUnit legacy{};
    unsigned legacyError = capacity;
    for (unsigned size = 32; size <= 64; ++size) {
        const unsigned target = size * payload, valid = divideUp(target, capacity);
        const unsigned error = valid * capacity - target;
        if (error >= legacyError) continue;
        const uint64_t excess = uint64_t(lineBytes / valid + 1) * error;
        const unsigned extra = excess >= capacity ? divideUp(excess, capacity) : 0;
        const unsigned extraDelay = divideUp(uint64_t(divideUp(extra, bytesPerPixel)) * clock, mode.pixel);
        legacy = {size, valid, size - valid + extraDelay + bufferMargin};
        legacyError = error;
    }
    const unsigned legacyTus = lineBytes / legacy.valid;
    const unsigned legacyPerLane = legacyTus / lanes;
    const bool legacyFits = fitsLine(legacy.delay, legacy.size, legacyPerLane, legacyPerLane * legacy.valid);
    const bool legacyEven = legacyTus % lanes == 0;
    if (legacyFits && !legacyError) {
        out = legacy;
        return true;
    }

    for (unsigned bufferPixels = margin; bufferPixels > 0; --bufferPixels) {
        TransferUnit moderated{};
        uint64_t bestError = uint64_t(legacyFits ? legacyError : capacity) << 16;
        const unsigned buffer = divideUp(uint64_t(bufferPixels) * clock, mode.pixel);
        for (unsigned size = 32; size <= 64; ++size) {
            const unsigned target = size * payload, valid = divideUp(target, capacity);
            if (valid < 2) continue;
            const unsigned upperError = valid * capacity - target;
            for (unsigned upper = 1; upper <= 15; ++upper) {
                for (unsigned lower = 1; lower <= 15; ++lower) {
                    const unsigned period = upper + lower, sum = period * valid - lower;
                    if (sum * capacity < target * period) continue;
                    const unsigned tus = lineBytes * period / sum, perLane = tus / lanes;
                    if (!perLane || (tus % lanes && (legacyEven || moderated.size))) continue;

                    const unsigned tail = perLane % period;
                    const unsigned upperTail = tail < upper ? tail : upper;
                    const unsigned sent = (perLane / period) * sum + tail * (valid - 1) + upperTail;
                    // The partial TU is sent without its normal filler symbols.
                    const unsigned tailBytes = lineBytes * period - tus * sum;
                    const uint64_t validScaled = sent * (lanes * period) + tailBytes;
                    const uint64_t lengthScaled = perLane * size * (lanes * period) + tailBytes;
                    const uint64_t available = validScaled * size * capacity;
                    const uint64_t needed = lengthScaled * target;
                    if (available < needed) continue;
                    const uint64_t error = ((available - needed) << 16) / lengthScaled;
                    if (error >= bestError) continue;

                    const unsigned avgError = sum * capacity - target * period;
                    const unsigned extra = divideUp(uint64_t(divideUp(lineBytes * period, sum)) * avgError,
                                                    period * capacity) + divideUp(uint64_t(upper) * lanes * upperError, capacity);
                    const unsigned extraDelay = divideUp(uint64_t(divideUp(extra, bytesPerPixel)) * clock, mode.pixel);
                    const unsigned delay = size - valid + 1 + extraDelay + buffer;
                    if (!fitsLine(delay, size, perLane, sent)) continue;
                    bestError = error;
                    moderated = {size, valid, delay, upper, lower};
                }
            }
        }
        if (moderated.size) {
            out = moderated;
            return true;
        }
    }
    if (!legacyFits) return false;
    out = legacy;
    return true;
}

inline bool detailedTiming(const uint8_t *dtd, Mode &out) {
    const unsigned pixel = (dtd[0] | unsigned(dtd[1]) << 8) * 10;
    const unsigned width = dtd[2] | (dtd[4] & 0xf0u) << 4, hblank = dtd[3] | (dtd[4] & 0xfu) << 8;
    const unsigned height = dtd[5] | (dtd[7] & 0xf0u) << 4, vblank = dtd[6] | (dtd[7] & 0xfu) << 8;
    const unsigned hf = dtd[8] | (dtd[11] & 0xc0u) << 2, hs = dtd[9] | (dtd[11] & 0x30u) << 4;
    const unsigned vf = (dtd[10] >> 4) | (dtd[11] & 0x0cu) << 2, vs = (dtd[10] & 0xfu) | (dtd[11] & 3u) << 4;
    // Progressive, separate digital sync, through the single external mixer.
    // Per-rate RGB8 payload and blanking limits are checked during selection.
    if (!pixel || (dtd[17] & 0x98) != 0x18 || !width || !height ||
        !hs || !vs || hf + hs >= hblank || vf + vs >= vblank) return false;
    if (uint64_t(pixel) * 1000 > uint64_t(width + hblank) * (height + vblank) * 61) return false;
    out = {pixel, width, height, hf, hs, hblank - hf - hs, vf, vs, vblank - vf - vs,
           !(dtd[17] & 2), !(dtd[17] & 4)};
    return true;
}

inline bool desktopMode(const Mode &mode) {
    return mode.width && mode.height && wideBusCompatible(mode) && mode.width<=2560 &&
           uint64_t((mode.width * 4 + 63) & ~63u) * mode.height <= (32u << 20);
}

// Choose a supported link rate for this one EDID timing before programming
// hardware. A failed training attempt must not silently change the mode.
inline uint8_t nativeRate(const Mode &mode,uint8_t maxRate) {
    if(!desktopMode(mode))return 0;
    constexpr uint8_t rates[]={UkeDpLink::kRbr,UkeDpLink::kHbr,UkeDpLink::kHbr2};
    for(uint8_t rate:rates) {
        TransferUnit tu{};
        if(rate<=maxRate && pixelRatio(streamPixelClock(mode,true),rate).m && transferUnit(mode,rate,tu))return rate;
    }
    return 0;
}

struct ModeSelection {
    Mode hbr2{}, hbr{}, rbr{};
    bool wideBus=false;

    const Mode &atRate(uint8_t rate) const {
        if (rate == UkeDpLink::kHbr2) return hbr2;
        return rate == UkeDpLink::kHbr ? hbr : rbr;
    }

    void consider(const Mode &mode) {
        if (wideBus && !desktopMode(mode)) return;
        const unsigned area = mode.width * mode.height;
        const auto select = [&](uint8_t rate, Mode &best) {
            if (area <= best.width * best.height) return;
            TransferUnit tu{};
            if (pixelRatio(streamPixelClock(mode, wideBus), rate).m && transferUnit(mode, rate, tu)) best = mode;
        };
        select(UkeDpLink::kHbr2, hbr2);
        select(UkeDpLink::kHbr, hbr);
        select(UkeDpLink::kRbr, rbr);
    }
    void timings(const uint8_t *block, unsigned first, unsigned end) {
        for (unsigned offset = first; offset + 18 <= end; offset += 18) {
            Mode mode{};
            if (detailedTiming(block + offset, mode)) consider(mode);
        }
    }
    void base(const uint8_t *block) {
        timings(block, 54, 126);
        if (block[35] & 0x20)
            consider({25200, 640, 480, 16, 96, 48, 10, 2, 33, true, true});
    }
    void cta(const uint8_t *block) {
        if (block[0] != 2 || block[2] < 4 || block[2] > 127) return;
        const unsigned firstTiming = block[2];
        for (unsigned offset = 4; offset < firstTiming;) {
            const unsigned tag = block[offset] >> 5, count = block[offset] & 31;
            ++offset;
            if (offset + count > firstTiming) return;
            if (tag == 2)
                for (unsigned i = 0; i < count; ++i)
                    switch (block[offset + i] & 0x7f) {
                    case 16: consider({148500, 1920, 1080, 88, 44, 148, 4, 5, 36, false, false}); break;
                    case 4: consider({74250, 1280, 720, 110, 40, 220, 5, 5, 20, false, false}); break;
                    }
            offset += count;
        }
        timings(block, firstTiming, 127);
    }
};
}
