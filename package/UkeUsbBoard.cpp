// SPDX-License-Identifier: GPL-2.0-only
// Cliffs USB2 device path from the OEM live FDT and MiCode uke-v-oss.
// WCD939x fixed peripheral route follows the Senemos 0006-uke-usb-v7-api reference.
#include "UkeUsbBoard.h"
#include "BootResetCore.h"
#include "GeniI2c.h"
#include "QcomEusb2Phy.h"
#include "QcomRpmh.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>

LCK_GRP_DECLARE(uke_apps_smmu_group, "uke-apps-smmu");
LCK_MTX_DECLARE(uke_apps_smmu_mutex, &uke_apps_smmu_group);
extern "C" volatile uint32_t native_usb_stats[54];

namespace {
void sync() {__asm__ volatile("dsb sy" ::: "memory");}

struct Registers {
    IOMemoryMap *mapping = nullptr;
    volatile uint32_t *base = nullptr;
    ~Registers() {OSSafeReleaseNULL(mapping);}
    bool map(uint64_t pa, uint64_t bytes) {
        auto *range = IODeviceMemory::withRange(pa, bytes);
        mapping = range ? range->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(range);
        base = mapping ? reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()) : nullptr;
        return base != nullptr;
    }
    uint32_t read(unsigned offset) const {return base[offset / 4];}
    void write(unsigned offset, uint32_t value) const {base[offset / 4] = value; sync();}
    void update(unsigned offset, uint32_t mask, uint32_t value) const {
        write(offset, (read(offset) & ~mask) | value);
    }
    bool wait(unsigned offset, uint32_t mask, uint32_t value, unsigned count = 200) const {
        for (unsigned i = 0; i < count; ++i) {
            if ((read(offset) & mask) == value) return true;
            IODelay(10);
        }
        return false;
    }
};

constexpr unsigned kCoreBcr = 0x39000, kPhyBcr = 0x12000;
constexpr unsigned kCoreGdsc = 0x39004;
constexpr unsigned kCoreClocks[] = {0x39088, 0x39018, 0x39024, 0x39028, 0x3908c};
Registers gGcc;

bool refClock() {
    // qcom_scm_io_readl, atomic SIP IO service. The OEM TCSR gate is in a secure aperture.
    constexpr uintptr_t pa = 0x1fb1118;
    register uint64_t x0 __asm__("x0") = 0xc2000501;
    register uint64_t x1 __asm__("x1") = 1;
    register uint64_t x2 __asm__("x2") = pa;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                    : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    printf("[usb] uke TCSR clkref scm=%lld gate=%08x\n", (long long)x0, (uint32_t)x1);
    return x0 == 0 && (x1 & 1);
}

// rpmh-rsc.c extracts the major version at bits 23:16. The board's DRV-2
// selection comes from its physical aperture, while bits 15:8 hold the minor version.
constexpr bool supportedRscVersion(uint32_t id) {return ((id >> 16) & 0xff) == 3;}
static_assert(supportedRscVersion(0x00030200), "Uke apps RSC DRV-2 reports version 3.2");
static_assert(!supportedRscVersion(0x00020200), "USB uses the version 3 status offsets");

bool voteRails() {
    Registers db, rsc;
    constexpr unsigned kDbSize = 0x20000, kDrvOffset = 0xd00, kUsbTcs = 2;
    if (!db.map(0x81c60000, kDbSize) || !rsc.map(0x17a20000, 0x10000)) return false;
    auto *copy = static_cast<uint8_t *>(IOMallocData(kDbSize));
    if (!copy) return false;
    for (unsigned i = 0; i < kDbSize; i += 4) {
        uint32_t word = db.read(i);
        memcpy(copy + i, &word, sizeof(word));
    }
    const uint32_t rscId = rsc.read(0);
    const auto layout = qcom_rsc_layout(rscId);
    const unsigned tcs = kDrvOffset + kUsbTcs * layout.tcs_stride;
    constexpr unsigned kV3TcsStatus = 0x28, kV3CmdStatus = 0x40;
    constexpr unsigned kTriggered = 1u << 24, kIssuedAndComplete = (1u << 8) | (1u << 16);
    bool ok = supportedRscVersion(rscId);
    auto submit = [&](const char *resource, const uint32_t *addr, const uint32_t *val, unsigned count) {
        const uint32_t status = rsc.read(tcs + kV3TcsStatus);
        const uint32_t control = rsc.read(tcs + layout.control);
        if (!status || (control & kTriggered)) {
            printf("[usb] uke rpmh busy resource=%s tcs=%u status=%08x control=%08x\n",
                   resource, kUsbTcs, status, control);
            return false;
        }
        QcomRpmhWrite writes[20];
        const unsigned n = qcom_rpmh_active_request(&layout, kUsbTcs, addr, val, count, writes, 20);
        if (!n) return false;
        for (unsigned i = 0; i < n; ++i) rsc.write(kDrvOffset + writes[i].offset, writes[i].value);
        for (unsigned i = 0; i < count; ++i) {
            const unsigned offset = tcs + kV3CmdStatus + i * layout.cmd_stride;
            if (!rsc.wait(offset, kIssuedAndComplete, kIssuedAndComplete, 5000)) {
                printf("[usb] uke rpmh timeout resource=%s cmd=%u address=%08x status=%08x tcs=%08x irq=%08x\n",
                       resource, i, addr[i], rsc.read(offset), rsc.read(tcs + kV3TcsStatus),
                       rsc.read(kDrvOffset + layout.irq_status));
                return false;
            }
        }
        const unsigned done = qcom_rpmh_finish(&layout, kUsbTcs, writes, 20);
        for (unsigned i = 0; i < done; ++i) rsc.write(kDrvOffset + writes[i].offset, writes[i].value);
        return true;
    };
    const struct {const char *name; unsigned mv;} rails[] = {
        {"ldob2", 880}, {"ldob4", 1200}, {"ldob7", 1800}, {"ldob17", 3075},
    };
    for (unsigned i = 0; ok && i < sizeof(rails) / sizeof(*rails); ++i) {
        QcomRpmhResource rail{};
        const int lookup = qcom_cmd_db_find(copy, kDbSize, rails[i].name, &rail);
        ok = lookup == 0;
        if (!ok) {
            printf("[usb] uke cmd-db resource=%s status=%d\n", rails[i].name, lookup);
        } else {
            const uint32_t addresses[] = {rail.address + 8, rail.address, rail.address + 4};
            const uint32_t values[] = {7, rails[i].mv, 1};
            ok = submit(rails[i].name, addresses, values, 3);
        }
        if (ok) native_usb_stats[9] |= 1u << i;
    }
    QcomRpmhResource xo{}, sn2{};
    if (ok && !qcom_cmd_db_find(copy, kDbSize, "xo.lvl", &xo) &&
        !qcom_cmd_db_find(copy, kDbSize, "SN2", &sn2) && sn2.level_count >= 3) {
        const auto unit = sn2.levels[0] | (uint32_t)sn2.levels[1] << 16;
        const uint64_t scaled = unit ? (((60000ull * sn2.levels[2] + 15) / 16) * 1000 + unit - 1) / unit : 0;
        const uint32_t vote = scaled > 0x3fff ? 0x3fff : static_cast<uint32_t>(scaled);
        const uint32_t addresses[] = {xo.address, sn2.address};
        const uint32_t values[] = {3, 1u << 30 | 1u << 29 | vote << 14 | vote};
        ok = vote && submit("xo.lvl/SN2", addresses, values, 2);
    } else {
        ok = false;
    }
    IOFreeData(copy, kDbSize);
    printf("[usb] uke rails votes=%x rsc=%08x complete=%u\n", native_usb_stats[9], rscId, ok);
    return ok;
}

bool repeater() {
    Registers core, owner, observer, channel;
    if (!core.map(0xc400000, 0x3000) || !owner.map(0xc42d000, 0x4000)) return false;
    const br_arbiter arb{core.base, owner.base, 0};
    const uint32_t version = core.read(0);
    const unsigned count = core.read(BR_FEATURES) & BR_FEATURES_APIDS_MASK;
    if (version < BR_VERSION_V7_MIN || version >= BR_VERSION_V8_MIN || !count || count > 1024)
        return false;
    unsigned apid = 0;
    while (apid < count) {
        if (((core.read(BR_APID_MAP + 4 * apid) >> 8) & 0xfff) == 0x7fd &&
            (owner.read(BR_CNFG_OWNER + 4 * apid) & 7) == arb.ee)
            break;
        ++apid;
    }
    if (apid == count || !observer.map(0xc440000, BR_OBS_APID_STRIDE * count) ||
        !channel.map(0xc500000, BR_CHNL_APID_STRIDE * count)) return false;
    auto read = [&](uint8_t reg, uint8_t &value) {
        return br_read8(observer.base, &arb, apid, reg, &value) == 0;
    };
    auto write = [&](uint8_t reg, uint8_t value) {
        return br_write8(channel.base, apid, reg, value) == 0;
    };
    uint8_t value = 0;
    if (!read(0x46, value) || !write(0x46, value | 0x80)) return false;
    // Normal device mode tuning from the applied eusb2-repeater@fd00 node.
    constexpr uint8_t tuning[] = {0, 0x0c, 3, 7, 2, 3, 0, 4, 2, 0, 3, 2};
    for (unsigned i = 0; i < sizeof tuning; ++i)
        if (!write(0x50 + i, tuning[i])) return false;
    constexpr uint8_t forceRegisters[] = {0xe8, 0xed};
    for (uint8_t reg : forceRegisters) {
        if (!read(reg, value) || !write(reg, value & ~0x40u)) return false;
    }
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        if (!read(8, value)) return false;
        native_usb_stats[8] = value | apid << 16;
        if (value & 0x80) return true;
        IODelay(100);
    }
    return false;
}

bool routeWcd() {
    Registers engine, pins, reset;
    if (!engine.map(0xa80000, 0x4000) || !pins.map(0xf134000, 0x2000) ||
        !reset.map(0xf177000, 0x1000)) return false;
    // GPIO52/53 mux selector 1 for QUP0 SE0, 2 mA pull-up. GPIO119 is the WCD reset.
    pins.update(0, 0x3ff, 7);
    pins.update(0x1000, 0x3ff, 7);
    reset.update(4, 2, 0);
    reset.update(0, 0x3ff, (1u << 9) | (7u << 6));
    IOSleep(20);
    reset.update(4, 2, 2);
    IOSleep(20);
    gGcc.update(0x52008, 0, 1u << 18 | 1u << 19 | 1u << 22);
    if (!gGcc.wait(0x18004, 1u << 31, 0)) return false;
    unsigned level = 0;
    for (; level < 8; ++level) {
        if ((gGcc.read(0x1802c + 4 * level) & 0x71f) <= 1) break;
    }
    if (level == 8 || ((engine.read(GENI_FW_REVISION) >> 8) & 0xff) != GENI_PROTO_I2C) return false;
    engine.update(0x28, 0, 0x7f);
    engine.update(0xe30, 0, 0xf);
    engine.write(0x24, 0x7f);
    engine.write(0x20, 1);
    engine.write(0xe18, 0);
    engine.write(0x7c, level);
    engine.write(GENI_CLK_CFG, 7u << 4 | 1u);
    engine.write(GENI_SCL_COUNTERS, 10u << 20 | 12u << 10 | 26);
    const GeniIo io = {
        [](void *ctx, uint32_t reg) {return static_cast<Registers *>(ctx)->read(reg);},
        [](void *ctx, uint32_t reg, uint32_t value) {static_cast<Registers *>(ctx)->write(reg, value);},
        &engine,
    };
    GeniI2c bus{};
    uint32_t cfg = 0;
    if (geni_i2c_prepare(&bus, &io, 0x0e, &cfg)) return false;
    auto finish = [&] {
        for (unsigned step = 0; step < 3; ++step) {
            for (unsigned i = 0; i < 100; ++i) {
                const int status = geni_i2c_poll(&bus);
                if (status != GeniPending) return status == GeniDone;
                IODelay(10);
            }
            if (geni_i2c_timeout(&bus) != GeniPending) return false;
        }
        return false;
    };
    auto write = [&](uint8_t reg, uint8_t val) {return !geni_i2c_start_write(&bus, reg, val) && finish();};
    auto read = [&](uint8_t reg) {return !geni_i2c_start_read_combined(&bus, reg) && finish();};
    if (!write(0, 4) || !read(0x3a)) return false;
    native_usb_stats[32] = bus.data;
    if (bus.data != 1) return false; // Harmonium 2.0, the reviewed peripheral route.
    const struct {uint16_t reg; uint8_t mask, value;} route[] = {
        {0x419, 0x10, 0}, {0x413, 3, 1}, {0x020, 9, 9},
        {0x063, 0x77, 0x77}, {0x01e, 0x0e, 0x0c}, {0x021, 0x0e, 0x0c}, {0x0a1, 0xff, 0xff},
        {0x419, 8, 0}, {0x419, 8, 8}, {0x403, 0xff, 0x80}, {0x419, 7, 5},
        {0x404, 0x3c, 0x14}, {0x415, 0xf8, 0xc0}, {0x403, 0x18, 0x18},
    };
    for (const auto &item : route) {
        const uint8_t reg = static_cast<uint8_t>(item.reg);
        if (!write(0, item.reg >> 8) || !read(reg) ||
            !write(reg, (bus.data & ~item.mask) | item.value) || !read(reg) ||
            (bus.data & item.mask) != item.value) return false;
    }
    native_usb_stats[32] |= 1u << 16;
    return true;
}

bool initPhy() {
    Registers phy, eud;
    if (!phy.map(0x88e3000, 0x1000) || !eud.map(0x88e2000, 0x1000) ||
        (eud.read(0) & 1u)) return false;
    gGcc.update(kPhyBcr, 1, 1);
    if (!gGcc.wait(kPhyBcr, 1, 1)) return false;
    IODelay(150);
    gGcc.update(kPhyBcr, 1, 0);
    if (!gGcc.wait(kPhyBcr, 1, 0)) return false;
    for (const auto &row : kQcomEusb2Init) {
        if (row.offset == 0x7c || row.offset == 0x78) continue; // Uke uses the OEM defaults for these fields.
        phy.update(row.offset, row.mask, row.value);
        if ((phy.read(row.offset) & row.mask) != row.value) {
            printf("[usb] uke eUSB2 write offset=%x expected=%x observed=%08x\n",
                   row.offset, row.value, phy.read(row.offset));
            return false;
        }
    }
    printf("[usb] uke eUSB2 common=%08x div=%08x ctrl=%08x por=%08x apb=%08x\n",
           phy.read(0x54), phy.read(0x5c), phy.read(0x64), phy.read(0x50), phy.read(0x130));
    return true;
}
} // namespace

bool uke_usb_reset_controller() {
    if (!gGcc.base) return false;
    // dwc3_msm_link_clk_reset: gate the link clocks, assert the external block reset, then restore clocks.
    for (unsigned offset : kCoreClocks) gGcc.update(offset, 1, 0);
    gGcc.update(kCoreBcr, 1, 1);
    const bool asserted = gGcc.wait(kCoreBcr, 1, 1);
    IOSleep(1);
    gGcc.update(kCoreBcr, 1, 0);
    const bool released = gGcc.wait(kCoreBcr, 1, 0);
    IODelay(1);
    bool clocked = true;
    for (unsigned offset : kCoreClocks) {
        gGcc.update(offset, 1, 1);
        clocked = gGcc.wait(offset, 0x80000001u, 1) && clocked;
    }
    IOSleep(10);
    printf("[usb] uke BCR assert=%u release=%u clocks=%u\n", asserted, released, clocked);
    return asserted && released && clocked;
}

int uke_usb2_hardware_init() {
    if (!gGcc.map(0x100000, 0x53000)) return -1;
    native_usb_stats[0] = 2;
    if (!voteRails()) return -2;
    native_usb_stats[0] = 3;
    gGcc.update(kCoreGdsc, 1, 0);
    if (!gGcc.wait(kCoreGdsc, 1u << 31, 1u << 31, 10000)) return -3;
    native_usb_stats[5] = gGcc.read(kCoreGdsc);
    native_usb_stats[0] = 4;
    // Core at the OEM's 200 MHz source, GPLL0/3. Preserve mock UTMI's firmware divisor.
    gGcc.update(0x39030, 0x71f, 0x105);
    gGcc.update(0x3902c, 0, 1);
    if (!gGcc.wait(0x3902c, 1, 0) || !refClock() || !uke_usb_reset_controller()) return -4;
    native_usb_stats[0] = 6;
    if (!repeater() || !routeWcd()) {
        printf("[usb] uke repeater=%08x wcd=%08x\n", native_usb_stats[8], native_usb_stats[32]);
        return -6;
    }
    native_usb_stats[0] = 7;
    if (!initPhy()) return -7;
    IOSleep(2);
    return 0;
}
