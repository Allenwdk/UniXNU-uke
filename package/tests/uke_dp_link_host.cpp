// SPDX-License-Identifier: GPL-2.0-only
#include "../UkeDpLinkPolicy.h"
#include "../UkeDpVideoMode.h"
#include "../UkeDpDpu.h"
#include "../UkeQmpDp.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using namespace UkeDpLink;
    Status status;
    status.bytes[0] = 0x01;
    assert(!status.clockRecovered() && !status.equalized());
    status.bytes[0] = 0x11;
    assert(status.clockRecovered() && !status.equalized());
    status.bytes[0] = 0x77;
    assert(!status.equalized());
    status.bytes[2] = 1;
    assert(status.equalized());

    for (unsigned request = 0; request < 256; ++request) {
        status.bytes[4] = request;
        const auto levels = SignalLevels::requested(status);
        assert(levels.swing <= 3 && levels.pre <= 3 && levels.swing + levels.pre <= 3);
        assert(kUkeDpSwing[levels.swing][levels.pre] != 0xff);
        assert(kUkeDpPreEmphasis[levels.swing][levels.pre] != 0xff);
        assert(kUkeDpHbr2Swing[levels.swing][levels.pre] != 0xff);
        assert(kUkeDpHbr2PreEmphasis[levels.swing][levels.pre] != 0xff);
    }
    status.bytes[4] = 0xc2; // Lane 0 requests swing 2, lane 1 requests pre-emphasis 3.
    assert(SignalLevels::requested(status).laneSet() == 0x0a);
    status.bytes[4] = 3;
    assert(SignalLevels::requested(status).laneSet() == 0x07);
    assert(trainingDelayUs(0, false) == 100 && trainingDelayUs(0, true) == 400);
    assert(trainingDelayUs(4, true) == 16000);
    std::puts("Uke DP training lane status, delays and all adjustment requests passed");

    uint8_t cta[128]{};
    cta[0] = 2;
    cta[2] = 7;
    cta[4] = 0x42;
    cta[5] = 0x80 | 16;
    cta[6] = 4;
    UkeDpVideo::ModeSelection modes;
    modes.cta(cta);
    assert(modes.hbr.width == 1920 && modes.hbr.height == 1080 && modes.hbr.pixel == 148500);
    assert(modes.rbr.width == 1280 && modes.rbr.height == 720 && modes.rbr.pixel == 74250);
    UkeDpVideo::TransferUnit unit{};
    assert(UkeDpVideo::exactTransferUnit(modes.hbr, kHbr, unit));
    assert(unit.size == 40 && unit.valid == 33 && unit.delay == 15);
    assert(!UkeDpVideo::exactTransferUnit(modes.hbr, kRbr, unit));
    assert(UkeDpVideo::exactTransferUnit(modes.rbr, kRbr, unit));
    assert(unit.size == 32 && unit.valid == 22 && unit.delay == 19);

    const uint8_t dtd[18] = {0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40,
                             0x58, 0x2c, 0x45, 0, 0, 0, 0, 0, 0, 0x1e};
    UkeDpVideo::Mode detailed{};
    assert(UkeDpVideo::detailedTiming(dtd, detailed));
    assert(detailed.htotal() == 2200 && detailed.vtotal() == 1125 && !detailed.hlow && !detailed.vlow);
    UkeDpVideo::ModeSelection malformed;
    cta[2] = 5;
    malformed.cta(cta);
    assert(!malformed.hbr.pixel && !malformed.rbr.pixel);
    std::puts("Uke DP EDID mode selection and video transfer units passed");

    using namespace UkeDpDpu;
    const auto regs = timing(detailed);
    assert(regs.hsync == (2200u << 16 | 44u) && regs.vperiod == 2475000 && regs.vwidth == 11000);
    assert(regs.hactive == (2111u << 16 | 192u) && regs.vstart == 90392 && regs.vend == 2466111);
    assert(regs.vaend == 2466391 && regs.config == (kActiveH | kActiveV) && !regs.prefetch);
    const auto wide=timing(detailed,true);
    assert(UkeDpVideo::wideBusCompatible(detailed));
    assert(UkeDpVideo::streamPixelClock(detailed,true)==74250);
    assert(wide.hsync==(1100u<<16 | 22u) && wide.vperiod==1237500 && wide.vwidth==5500);
    assert(wide.hactive==(1099u<<16 | 140u) && wide.vstart==49640 &&
           wide.vend==1237499 && wide.vaend==1237639);
    const auto msa=UkeDpVideo::streamRatio(11,200,true,kHbr);
    assert(msa.mvid==17930 && msa.nvid==32600);
    assert(uint64_t(msa.mvid)*270000==uint64_t(msa.nvid)*detailed.pixel);
    auto odd=detailed;
    ++odd.hfront;
    assert(!UkeDpVideo::wideBusCompatible(odd));
    UkeDpVideo::ModeSelection wideModes;
    wideModes.wideBus=true;
    wideModes.consider(odd);
    assert(!wideModes.hbr.pixel);
    wideModes.consider(detailed);
    assert(wideModes.hbr.pixel==148500);
    std::puts("Uke DP OEM wide-bus timings, clock ratio and mode filtering passed");

    // 241.70 MHz 1440p EDID fixture, matched against Xiaomi's dp_panel_calc_tu_test.
    uint8_t edid1440[128]{};
    const uint8_t dtd1440[18] = {0x6a, 0x5e, 0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50,
                                 0x30, 0x20, 0x35, 0, 0, 0, 0, 0, 0, 0x1a};
    std::memcpy(edid1440 + 54, dtd1440, sizeof dtd1440);
    UkeDpVideo::ModeSelection native;
    native.wideBus = true;
    native.base(edid1440);
    cta[2] = 7;
    native.cta(cta);
    const auto &qhd = native.atRate(kHbr2);
    assert(qhd.width == 2560 && qhd.height == 1440 && qhd.pixel == 241700);
    assert(UkeDpVideo::nativeRate(qhd,kHbr2)==kHbr2);
    assert(UkeDpVideo::nativeRate(qhd,kHbr)==0 && UkeDpVideo::nativeRate(qhd,kRbr)==0);
    assert(UkeDpVideo::nativeRate(native.atRate(kHbr),kHbr2)==kHbr);
    assert(UkeDpVideo::nativeRate(native.atRate(kRbr),kHbr2)==kRbr);
    assert(qhd.htotal() == 2720 && qhd.vtotal() == 1481);
    assert(native.atRate(kHbr).pixel == 148500 && native.atRate(kRbr).pixel == 74250);
    assert(!UkeDpVideo::transferUnit(qhd, kRbr, unit) && !UkeDpVideo::transferUnit(qhd, kHbr, unit));
    assert(!UkeDpVideo::exactTransferUnit(qhd, kHbr2, unit));
    assert(UkeDpVideo::transferUnit(qhd, kHbr2, unit));
    assert(unit.size == 34 && unit.valid == 23 && unit.delay == 26);
    assert(unit.upperCount == 5 && unit.lowerCount == 1 && unit.boundary2() == 0x0015002d);
    assert(ukeDpPhyVcoDividerCode(kHbr2) == 2 && UkeDpVideo::pixelParentClock(kHbr2) == 1350000);
    const auto qhdClock = UkeDpVideo::pixelRatio(UkeDpVideo::streamPixelClock(qhd, true), kHbr2);
    assert(qhdClock.m == 2417 && qhdClock.n == 27000);
    const auto qhdMsa = UkeDpVideo::streamRatio(qhdClock.m, qhdClock.n, true, kHbr2);
    assert(qhdMsa.mvid == 24170 && qhdMsa.nvid == 54000);
    assert(uint64_t(qhdMsa.mvid) * UkeDpVideo::linkClock(kHbr2) == uint64_t(qhdMsa.nvid) * qhd.pixel);
    const auto approximate = UkeDpVideo::pixelRatio(117295, kHbr2);
    assert(approximate.m == 4713 && approximate.n == 54244);
    auto overBudget = qhd;
    overBudget.pixel = 360000;
    assert(!UkeDpVideo::transferUnit(overBudget, kHbr2, unit));
    auto noBlank = qhd;
    noBlank.hfront = 1;
    noBlank.hsync = 2;
    noBlank.hback = 1;
    assert(!UkeDpVideo::transferUnit(noBlank, kHbr2, unit));
    std::puts("Uke DP HBR2 2560x1440 OEM TU, PLL, MSA and HBR/RBR fallback passed");

    auto shortBlank = detailed;
    shortBlank.vback = 10;
    const auto prefetch = timing(shortBlank);
    assert(prefetch.config & kFetch);
    assert(prefetch.prefetch == (shortBlank.vtotal() - shortBlank.vfront) * shortBlank.htotal() + 1);

    struct Registers {
        unsigned occupied = ~0u;
        uint32_t value = 0;
        uint32_t mux = 2;
        uint32_t read(unsigned offset) const {
            if (offset == 0x35000 + 0x25c) return mux;
            return offset == occupied ? value : 0;
        }
    } mdp;
    assert(!busyResource(mdp));
    mdp = {kTiming, 1};
    assert(!std::strcmp(busyResource(mdp), "INTF0"));
    mdp = {0x35000 + 0x26c, 1};
    assert(!std::strcmp(busyResource(mdp), "PP2"));
    mdp.mux = 3;
    assert(!std::strcmp(busyResource(mdp), "PP3"));
    mdp = {0x16000 + 0xe4, kMerge1Flush};
    assert(!std::strcmp(busyResource(mdp), "MERGE_3D1"));
    mdp = {0x15000 + 0x70, 1u << 8};
    assert(!std::strcmp(busyResource(mdp), "DMA0 mixer"));
    mdp = {0x16000 + 8, 1u << 3};
    assert(!std::strcmp(busyResource(mdp), "LM2"));
    mdp = {kCtl + 0xfc, 1u << 17};
    assert(!std::strcmp(busyResource(mdp), "CTL2"));
    std::puts("Uke DP DPU timing and resource conflict tests passed");

    assert(kDmaBaseStage == 0x00040000 && kInitialFlush == 0xc0820900);
    assert(kPeriphFlushReg==0x17128 && kFlipFlush==0x40000800);
    assert(kXinLimitMask == 0x0000ff00 && kXinReadLimit == 0x00002800);
    assert(kXinQosMask == 0x70 && xinQosValue(0) == 0x40 && xinQosValue(7) == 0x60);
    assert(xinQosOffset(kVbifQosRequest, 0) == 0x550 && xinQosOffset(kVbifQosLevel, 7) == 0x5c8);
    const uint32_t previousLimit = 0x40302010, previousQos = 0x76543210;
    assert(((previousLimit & ~kXinLimitMask) | kXinReadLimit) == 0x40302810);
    assert(((previousQos & ~kXinQosMask) | xinQosValue(7)) == 0x76543260);
    assert(kPp2Fifo == 0x354 && pp2FifoSize(2560) == 0x0a000000);
    assert(((0x01000200u & ~kPp2FifoMask) | pp2FifoSize(2560)) == 0x0a000200);
    std::puts("Uke DP solo stage, shared merge guard and VBIF XIN1 register masks passed");
    std::puts("Uke DP native EDID without resolution fallback and DP peripheral commit masks passed");
}
