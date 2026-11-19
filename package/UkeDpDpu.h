// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "UkeDpVideoMode.h"

namespace UkeDpDpu {
// SM7675 DPU catalog, offsets relative to 0x0ae01000. Use INTF0 for
// DPTX0 stream 0 and leave the panel's VIG0/1, LM0/1 and INTF1/2 alone.
constexpr unsigned kIntf = 0x34000, kMixer = 0x46000, kCtl = 0x17000, kDma = 0x24000;
constexpr unsigned kMerge = 0x4f000; // PP2/PP3 share MERGE_3D1.
constexpr unsigned kTiming = kIntf, kIntfStatus = kIntf + 0x26c, kFrame = kIntf + 0x0ac;
constexpr unsigned kFlush = kCtl + 0x018, kReset = kCtl + 0x030, kSrc = kDma + 0x014;
constexpr uint32_t kDmaFlush = 1U << 11;
// sde_encoder: every DP frame includes the peripheral flush, even with no DSC.
constexpr unsigned kPeriphFlushReg = kCtl + 0x128;
constexpr uint32_t kDpPeriphFlush = 1U << 30, kFlipFlush = kDmaFlush | kDpPeriphFlush;
constexpr uint32_t kInitialFlush = (1U << 31) | (1U << 23) | (1U << 17) | (1U << 8) | kFlipFlush;
constexpr uint32_t kDmaBaseStage = 1U << 18, kMerge1Flush = 1U << 1;
// DMA0 on the first blend stage over LM's black border rather than BASE.
constexpr uint32_t kDmaOverBorder = (1U << 24) | (2U << 18);
constexpr uint32_t kMixerCommit = (1U << 17) | (1U << 8);
constexpr unsigned kVig0 = 0x4000, kVig0Xin = 0;
constexpr uint32_t kVig0Fetch = 1U << 16, kVig0Flush = 1U;
constexpr uint32_t kVig0BaseStage = 1U, kVig0OverBorder = (1U << 24) | 2U;
constexpr unsigned kQseed = 0xa00, kCsc10 = 0x1a00;
constexpr uint32_t kXrgb2101010 = 0x000276ff, kArgb8888 = 0x000237ff;
constexpr uint32_t kUnpackBgra = 0x03020001;
constexpr uint32_t kQseedBilinearRgb = (1U << 31) | (3U << 16) | 1U;
constexpr uint32_t kCliffsQseedPreload = 0x02040204; // rev 0x3003: H=4, V=2.
constexpr uint32_t kVigXr10Scale = (1023 * 512 + 255) / 510; // OEM CSC S4.9.
constexpr uint32_t kVigXr10Bias = 0x10000 - 384;             // OEM signed 16-bit pre-bias.

constexpr bool xr10Geometry(unsigned sw,unsigned sh,unsigned dw,unsigned dh) {
    return dw && dh && sw <= 3840 && sh <= 2160 &&
        ((sw == dw && sh == dh) || (sw * 2 == dw * 3 && sh * 2 == dh * 3));
}

constexpr uint32_t qseedPhase(unsigned source,unsigned destination) {
    return uint32_t((uint64_t(source) << 21) / destination);
}
constexpr unsigned kPp2Fifo = 0x354; // MDP top PPB_FIFO_SIZE, PP2 in upper half.
constexpr uint32_t kPp2FifoMask = 0xffff0000u;
constexpr uint32_t pp2FifoSize(unsigned width) {
    // One available mode and one mixer. Cliffs needs four latency lines,
    // with four pixels per entry, measured before the wide-bus divider.
    return (width * 4 / 4) << 16;
}
constexpr uint32_t kActiveH = 1U << 29, kActiveV = 1U << 30, kFetch = 1U << 31;

struct Write {unsigned offset; uint32_t value;};

// SM7675 VBIF RT rules in the live DT. DMA0 is XIN1.
constexpr unsigned kXin = 1, kVbifReadLimit = 0xb0, kVbifHalt = 0x200;
constexpr unsigned kVbifQosRequest = 0x550, kVbifQosLevel = 0x590;
constexpr uint32_t kXinLimitMask = 0xffu << (8 * kXin), kXinReadLimit = 0x28u << (8 * kXin);
constexpr uint32_t kXinQosMask = 7u << (4 * kXin);
constexpr uint8_t kRtPriority[] = {4, 4, 5, 5, 5, 5, 5, 6};
constexpr unsigned xinQosOffset(unsigned base, unsigned priority) {return base + 8 * priority;}
constexpr uint32_t xinQosValue(unsigned priority,unsigned xin=kXin) {return kRtPriority[priority] << (4 * xin);}

// Qualcomm sde_hw_intf_setup_timing_engine for an uncompressed solo DP
// stream. Horizontal start and end are inclusive.
struct Timing {
    uint32_t hsync, vperiod, vwidth, hactive, vstart, vend, vaend, config, prefetch;
};

constexpr Timing timing(UkeDpVideo::Mode mode,bool wideBus=false) {
    if(wideBus) {
        // sde_encoder_phys_vid: move the blanking to the top and left,
        // then express horizontal coordinates in two-pixel bus cycles.
        mode.hback=(mode.hback+mode.hfront)/2;
        mode.hfront=0;
        mode.hsync/=2;
        mode.width/=2;
        mode.vback+=mode.vfront;
        mode.vfront=0;
    }
    const unsigned line = mode.htotal(), total = mode.vtotal();
    const unsigned hstart = mode.hback + mode.hsync, vstart = mode.vback + mode.vsync;
    const unsigned first = vstart * line + hstart;
    const unsigned last = (total - mode.vfront) * line - mode.hfront - 1;
    const unsigned shortage = vstart < 24 ? 24 - vstart : 0;
    const unsigned fetchLines = shortage < mode.vfront ? shortage : mode.vfront;
    return {(line << 16) | mode.hsync, total * line, mode.vsync * line,
            ((hstart + mode.width - 1) << 16) | hstart, first, last, first + mode.height * line - 1,
            kActiveH | kActiveV | (fetchLines ? kFetch : 0), fetchLines ? (total - fetchLines) * line + 1 : 0};
}

template <typename Registers>
const char *busyResource(const Registers &mdp) {
    if ((mdp.read(kTiming) | mdp.read(kIntfStatus)) & 1) return "INTF0";
    constexpr unsigned interfaces[] = {0x35000, 0x36000, 0x37000};
    for (unsigned intf : interfaces) {
        if (!(mdp.read(intf + 0x26c) & 1)) continue;
        const unsigned mux = mdp.read(intf + 0x25c) & 0xf;
        if (mux == 2) return "PP2";
        if (mux == 3) return "PP3";
    }
    constexpr unsigned controls[] = {0x15000, 0x16000, kCtl, 0x18000};
    for (unsigned ctl : controls) {
        if (mdp.read(ctl + 0xf4) & 1) return "INTF0 CTL";
        if (mdp.read(ctl + 0xfc) & 1) return "DMA0 fetch";
        if (mdp.read(ctl + 0xe4) & kMerge1Flush) return "MERGE_3D1";
        if (ctl == kCtl && (mdp.read(ctl + 0xe4) || mdp.read(ctl + 0xe8) || mdp.read(ctl + 0xec) ||
                            mdp.read(ctl + 0xf0) || mdp.read(ctl + 0xf4) || mdp.read(ctl + 0xf8) ||
                            mdp.read(ctl + 0xfc) || mdp.read(kFlush) || (mdp.read(kReset) & 1)))
            return "CTL2";
        for (unsigned lm = 0; lm < 4; ++lm) {
            const uint32_t base = mdp.read(ctl + lm * 4);
            const uint32_t ext = mdp.read(ctl + 0x40 + lm * 4);
            const uint32_t ext2 = mdp.read(ctl + 0x70 + lm * 4);
            if ((base & (7u << 18)) || (ext & (1u << 16)) || (ext2 & (0xfu << 8))) return "DMA0 mixer";
            const uint32_t ext3 = mdp.read(ctl + 0xa0 + lm * 4);
            const uint32_t ext4 = mdp.read(ctl + 0xb8 + lm * 4);
            if (lm == 2 && ((base & ~(1u << 24)) || ext || ext2 || ext3 || ext4)) return "LM2";
            if (ctl == kCtl && lm != 2 && ((base & ~(1u << 24)) || ext || ext2 || ext3 || ext4))
                return "CTL2 mixer";
        }
    }
    return nullptr;
}

template <typename Registers>
bool vig0Available(const Registers &mdp) {
    for(unsigned ctl=0x15000;ctl<=0x18000;ctl+=0x1000) {
        if(mdp.read(ctl+0xfc)&kVig0Fetch)return false;
        for(unsigned lm=0;lm<4;++lm) {
            if((mdp.read(ctl+lm*4)&7u) || (mdp.read(ctl+0x40+lm*4)&1u) ||
               (mdp.read(ctl+0xa0+lm*4)&0xfu))return false;
        }
    }
    return true;
}
}
