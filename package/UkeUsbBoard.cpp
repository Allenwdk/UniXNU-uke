// SPDX-License-Identifier: GPL-2.0-only
// Cliffs USB device path from the OEM live FDT and MiCode uke-v-oss.
// WCD939x fixed peripheral route follows the Senemos 0006-uke-usb-v7-api reference.
#include "UkeUsbBoard.h"
#include "BootResetCore.h"
#include "GeniI2c.h"
#include "NativeExternalDisplay.h"
#include "QcomEusb2Phy.h"
#include "QcomRpmh.h"
#include "UkeDpDpu.h"
#include "UkeDpLinkPolicy.h"
#include "UkeDpVideoMode.h"
#include "UkeQmpDp.h"
#include "UkeQmpUsb3.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>

IOReturn uke_dp_set_external_display(const NativeExternalDisplayLink *);

LCK_GRP_DECLARE(uke_apps_smmu_group, "uke-apps-smmu");
LCK_MTX_DECLARE(uke_apps_smmu_mutex, &uke_apps_smmu_group);
extern "C" volatile uint32_t native_usb_stats[54];
extern "C" uint32_t uke_ufs_dpu_map_external(unsigned slot, const uint64_t *pages, unsigned count)
    __attribute__((weak_import));
extern "C" uint32_t native_ufs_dpu_live(unsigned reg) __attribute__((weak_import));

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
    // The OEM tcsrcc-pineapple driver uses a normal MMIO regmap and clk_branch2_ops.
    constexpr uintptr_t tcsrccBase = 0x01f40000;
    constexpr unsigned usb2ClkrefOffset = 0x000b1118;
    constexpr uintptr_t pa = tcsrccBase + usb2ClkrefOffset;
    static_assert(pa == 0x01ff1118, "Cliffs USB2 TCSR gate address");
    constexpr uintptr_t page = pa & ~uintptr_t(0xfff);
    constexpr unsigned reg = static_cast<unsigned>(pa - page);
    constexpr uint32_t enable = 1u;
    Registers tcsr;
    if (!tcsr.map(page, 0x1000)) {
        printf("[usb] uke TCSR clkref pa=%08x map failed\n", static_cast<uint32_t>(pa));
        return false;
    }
    printf("[usb] uke TCSR clkref pa=%08x MMIO read\n", static_cast<uint32_t>(pa));
    const uint32_t before = tcsr.read(reg);
    if (!(before & enable)) {
        tcsr.write(reg, before | enable);
        IODelay(10); // BRANCH_HALT_DELAY
    }
    const uint32_t after = tcsr.read(reg);
    printf("[usb] uke TCSR clkref before=%08x after=%08x\n", before, after);
    return (after & enable) != 0;
}

// rpmh-rsc.c extracts the major version at bits 23:16. The board's DRV-2
// selection comes from its physical aperture, while bits 15:8 hold the minor version.
constexpr bool supportedRscVersion(uint32_t id) {return ((id >> 16) & 0xff) == 3;}
static_assert(supportedRscVersion(0x00030200), "Uke apps RSC DRV-2 reports version 3.2");
static_assert(!supportedRscVersion(0x00020200), "USB uses the version 3 status offsets");

bool voteRails(bool superSpeed) {
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
    const struct {const char *name; unsigned mv; bool superSpeed;} rails[] = {
        {"ldob2", 880, false}, {"ldob4", 1200, false}, {"ldob7", 1800, false},
        {"ldob17", 3075, false}, {"ldob3", 880, true},
    };
    for (unsigned i = 0; ok && i < sizeof(rails) / sizeof(*rails); ++i) {
        if (rails[i].superSpeed != superSpeed) continue;
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
        const uint64_t peak = superSpeed ? 2500000 : 60000;
        const uint64_t scaled = unit ? (((peak * sn2.levels[2] + 15) / 16) * 1000 + unit - 1) / unit : 0;
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

bool repeater(bool host) {
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
    // Device and host properties of the effective eusb2-repeater@fd00 node.
    constexpr uint8_t deviceTuning[] = {0, 0x0c, 3, 7, 2, 3, 0, 4, 2, 0, 3, 2};
    constexpr uint8_t hostTuning[] = {0, 0x08, 5, 7, 2, 1, 0, 6, 2, 0, 3, 2};
    const auto &tuning = host ? hostTuning : deviceTuning;
    for (unsigned i = 0; i < sizeof tuning; ++i)
        if (!write(0x50 + i, tuning[i])) return false;
    // Program the forced value before enabling its override.
    constexpr uint8_t forceRegisters[] = {0xed, 0xe8};
    for (uint8_t reg : forceRegisters) {
        const uint8_t force = host ? 0x40 : 0;
        if (!read(reg, value) || !write(reg, (value & ~0x40u) | force) ||
            !read(reg, value) || (value & 0x40u) != force) return false;
    }
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        if (!read(8, value)) return false;
        native_usb_stats[8] = value | apid << 16;
        if (value & 0x80) return true;
        IODelay(100);
    }
    return false;
}

class WcdRegmap {
    GeniI2c &bus;
    bool finish() {
        for (unsigned step = 0; step < 3; ++step) {
            for (unsigned i = 0; i < 100; ++i) {
                const int status = geni_i2c_poll(&bus);
                if (status != GeniPending) return status == GeniDone;
                IODelay(10);
            }
            if (geni_i2c_timeout(&bus) != GeniPending) return false;
        }
        return false;
    }
    bool write(uint8_t reg, uint8_t value) {return !geni_i2c_start_write(&bus, reg, value) && finish();}
    // OEM geni_i2c_fifo_xfer issues WRITE with STOP_STRETCH, followed by READ.
    bool readByte(uint8_t reg) {return !geni_i2c_start_read(&bus, reg) && finish();}
public:
    explicit WcdRegmap(GeniI2c &engine) : bus(engine) {}
    bool read(uint16_t reg, uint8_t &value) {
        if (!write(0, reg >> 8) || !readByte(static_cast<uint8_t>(reg))) return false;
        value = bus.data;
        return true;
    }
    bool update(uint16_t reg, uint8_t mask, uint8_t value) {
        const auto address = static_cast<uint8_t>(reg);
        return write(0, reg >> 8) && readByte(address) &&
               write(address, (bus.data & ~mask) | value) && readByte(address) &&
               (bus.data & mask) == value;
    }
};

bool programWcd(GeniI2c &bus, void *) {
    WcdRegmap regs(bus);
    auto failure = [&](const char *step, unsigned reg) {
        printf("[usb] uke WCD %s reg=%03x error=%u irq=%08x rx=%02x\n",
               step, reg, bus.error, bus.error_status, bus.data);
        return false;
    };
    constexpr uint16_t chipId1 = 0x439;
    uint8_t revision;
    if (!regs.read(chipId1, revision)) return failure("revision read", chipId1);
    native_usb_stats[32] = revision;
    printf("[usb] uke WCD CHIP_ID1=%02x\n", revision);
    if (revision != 1) return false; // Harmonium 2.0, the reviewed peripheral route.
    const struct {uint16_t reg; uint8_t mask, value;} route[] = {
        {0x419, 0x10, 0}, {0x413, 3, 1}, {0x020, 9, 9},
        {0x063, 0x77, 0x77}, {0x01e, 0x0e, 0x0c}, {0x021, 0x0e, 0x0c}, {0x0a1, 0xff, 0xff},
        {0x419, 8, 0}, {0x419, 8, 8}, {0x403, 0xff, 0x80}, {0x419, 7, 5},
        {0x404, 0x3c, 0x14}, {0x415, 0xf8, 0xc0}, {0x403, 0x18, 0x18},
    };
    for (const auto &item : route)
        if (!regs.update(item.reg, item.mask, item.value)) return failure("route", item.reg);
    native_usb_stats[32] |= 1u << 16;
    return true;
}

bool programWcdAux(GeniI2c &bus, void *context) {
    const int lane = *static_cast<const int *>(context);
    WcdRegmap regs(bus);
    constexpr uint16_t switchEnable = 0x403, switchSelect = 0x404, switchStatus = 0x407;
    constexpr uint8_t auxEnable = 0x60, auxSelect = 0xc0;
    const uint8_t selection = lane < 0 ? 0 : lane == 0 ? 0x40 : 0x80;
    uint8_t observed = 0;
    if (regs.update(switchEnable, auxEnable, 0) && regs.update(switchSelect, auxSelect, selection)) {
        if (lane < 0) return true;
        // Harmonium 2.0 DP AUX leakage canceller and break-before-make.
        if (regs.update(0x81, 7, 1) && regs.update(0x80, 0xe0, 0xe0) &&
            regs.update(0x82, 0xe0, 0xe0)) {
            IODelay(35);
            if (regs.update(switchEnable, auxEnable, auxEnable)) {
                IODelay(15);
                if (regs.read(switchStatus, observed) && (observed & 0xcc) == (lane == 0 ? 0x48 : 0x84)) {
                    if (native_legacy_dp_debug()) printf("[dp route] WCD AUX lane=%d status=%02x\n", lane, observed);
                    return true;
                }
            }
        }
    }
    // Only open the SBU switches on failure. USB DP/DM and EQ remain owned by USB.
    const bool isolated = regs.update(switchEnable, auxEnable, 0) &&
                          regs.update(switchSelect, auxSelect, 0);
    printf("[dp route] WCD AUX failed lane=%d status=%02x isolated=%u\n", lane, observed, isolated);
    return false;
}

bool routeWcd() {
    Registers reset;
    if (!reset.map(0xf177000, 0x1000)) {
        printf("[usb] uke WCD reset GPIO map failed\n");
        return false;
    }
    // GPIO119 is the WCD reset only. SE0 pins, GCC votes and timing belong to the shared backlight owner.
    reset.update(4, 2, 0);
    reset.update(0, 0x3ff, (1u << 9) | (7u << 6));
    IOSleep(20);
    reset.update(4, 2, 2);
    IOSleep(20);

    IOReturn result = kIOReturnNotReady;
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        result = uke_backlight_with_wcd_bus(programWcd);
        if (result != kIOReturnBusy && result != kIOReturnNotReady) break;
        IOSleep(10);
    }
    printf("[usb] uke WCD shared SE0 result=%08x revision_route=%08x\n",
           static_cast<uint32_t>(result), native_usb_stats[32]);
    return result == kIOReturnSuccess;
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

// Follow the QMP COM/PCS power-up sequence, with the OEM Cliffs resources and
// hardware port select on GPIO122. The stopped USB role selects USB-only or
// concurrent USB3 + DP before releasing the shared PHY reset.
bool initSuperSpeedPhy(int lane, bool displayPort) {
    constexpr uint32_t enable = 1u, halt = 1u << 31;
    constexpr uint32_t powerUpComplete = 1u << 16, retainRegisters = 1u << 11;
    constexpr unsigned gdsc = 0x50018, gdscCfg = 0x5001c, globalBcr = 0x50008, phyBcr = 0x50000;
    constexpr unsigned auxCmd = 0x39070, auxCfg = 0x39074;
    constexpr unsigned aux = 0x39060, comAux = 0x39064, pipe = 0x39068, pipeMux = 0x3906c;
    constexpr unsigned refGate = 0x108, gpioConfig = 0x3ff;
    constexpr unsigned comMode = 0x0, comReset = 0x4, comPower = 0x8;
    constexpr unsigned comSwi = 0xc, comTypec = 0x10, resetOverride = 0x1c;
    constexpr unsigned pcsReset = 0x1c00, pcsStatus = 0x1c14;
    constexpr unsigned pcsPower = 0x1c40, pcsStart = 0x1c44;
    constexpr uint32_t startBits = 3u, phyStatus = 1u << 6;
    Registers tcsr, tlmm, phy;
    if (!tcsr.map(0x01ff1000, 0x1000) || !tlmm.map(0x0f17a000, 0x1000) ||
        !phy.map(0x088e8000, 0x3000)) {
        printf("[usb] uke QMP map failed\n");
        return false;
    }
    const uint32_t gpioBefore = tlmm.read(0), refBefore = tcsr.read(refGate);
    const uint32_t gdscBefore = gGcc.read(gdsc), cfgBefore = gGcc.read(auxCfg);
    const uint32_t auxBefore = gGcc.read(aux), comBefore = gGcc.read(comAux);
    const uint32_t pipeBefore = gGcc.read(pipe), muxBefore = gGcc.read(pipeMux);
    bool phyAccessible = false;
    const char *phase = "gdsc";
    auto start = [&] {
        gGcc.update(gdsc, enable, 0);
        // The board advertises qcom,support-cfg-gdscr and qcom,retain-regs.
        IODelay(1);
        if (!gGcc.wait(gdscCfg, powerUpComplete, powerUpComplete, 10000)) return false;
        gGcc.update(gdsc, retainRegisters, retainRegisters);
        native_usb_stats[6] = gGcc.read(gdsc);

        phase = "aux";
        tcsr.update(refGate, enable, enable);
        IODelay(10);
        tlmm.update(0, gpioConfig, 1u | (1u << 2)); // Pull down, 2 mA, usb0_phy_ps input
        gGcc.update(auxCfg, 0x71f, 1u); // 19.2 MHz TCXO, /1
        gGcc.update(auxCmd, 0, enable);
        if (!(tcsr.read(refGate) & enable) || (tlmm.read(0) & gpioConfig) != 5u ||
            !gGcc.wait(auxCmd, enable, 0)) return false;
        gGcc.update(aux, enable, enable);
        gGcc.update(comAux, enable, enable);
        if (!gGcc.wait(aux, halt | enable, enable) || !gGcc.wait(comAux, halt | enable, enable)) return false;

        phase = "reset";
        gGcc.update(pipe, enable, 0);
        gGcc.update(pipeMux, 3u, 0); // PHY supplies the PIPE clock
        gGcc.update(globalBcr, enable, enable);
        gGcc.update(phyBcr, enable, enable);
        const bool asserted = gGcc.wait(globalBcr, enable, enable) && gGcc.wait(phyBcr, enable, enable);
        IODelay(20);
        gGcc.update(phyBcr, enable, 0);
        gGcc.update(globalBcr, enable, 0);
        if (!asserted || !gGcc.wait(globalBcr, enable, 0) || !gGcc.wait(phyBcr, enable, 0)) return false;
        phyAccessible = true;
        phy.write(comPower, 1);
        phy.write(comReset, 1);
        phy.write(comReset, 0);
        phy.write(resetOverride, 0xf);
        const uint32_t orientation = lane < 0 ? 0u : 2u | static_cast<unsigned>(lane);
        phy.write(comTypec, orientation);
        const uint32_t mode = displayPort ? 3u : 1u; // USB3_MODE and DP_MODE.
        phy.write(comMode, mode);
        phy.write(resetOverride, 0);
        phy.update(comSwi, 3u, 0);
        if ((phy.read(comTypec) & 3u) != orientation || (phy.read(comMode) & 3u) != mode) return false;
        phy.write(pcsPower, 1);
        for (const auto &entry : kUkeQmpUsb3Init) phy.write(entry.offset, entry.value);

        // Linux enables PIPE before releasing PCS reset and waiting for PHYSTATUS.
        // This branch uses BRANCH_HALT_DELAY: its PHY parent can start later.
        phase = "pipe";
        gGcc.update(pipe, enable, enable);
        IODelay(10);
        if (!(gGcc.read(pipe) & enable) || (gGcc.read(pipeMux) & 3u) != 0) return false;

        phase = "pcs";
        phy.write(pcsReset, 0);
        phy.write(pcsStart, startBits);
        // Cliffs PHY byte registers may repeat the low byte across the MMIO word.
        return phy.wait(pcsStatus, phyStatus, 0, 1000) &&
               (phy.read(pcsReset) & enable) == 0 &&
               (phy.read(pcsStart) & startBits) == startBits &&
               (phy.read(pcsPower) & enable) != 0;
    };
    const bool ok = start();
    native_usb_stats[33] = phyAccessible ? phy.read(pcsStatus) : ~0u;
    printf("[usb] uke QMP ready=%u phase=%s lane=%d gdsc=%08x/%08x pcs=%08x pipe=%08x mux=%08x\n", ok, phase, lane,
           gGcc.read(gdsc), gGcc.read(gdscCfg), native_usb_stats[33], gGcc.read(pipe), gGcc.read(pipeMux));
    if (phyAccessible) {
        printf("[usb] uke QMP COM mode=%08x typec=%08x; PCS reset=%08x start=%08x power=%08x\n",
               phy.read(comMode), phy.read(comTypec), phy.read(pcsReset), phy.read(pcsStart), phy.read(pcsPower));
    }
    if (ok) return true;
    if (phyAccessible) {
        phy.write(pcsStart, 0);
        phy.write(pcsReset, 1);
        phy.write(pcsPower, 0);
        phy.write(comPower, 0);
    }
    gGcc.update(pipe, enable, 0);
    gGcc.update(pipeMux, 3u, muxBefore & 3u);
    gGcc.update(pipe, enable, pipeBefore & enable);
    gGcc.update(aux, enable, auxBefore & enable);
    gGcc.update(comAux, enable, comBefore & enable);
    gGcc.update(auxCfg, 0x71f, cfgBefore & 0x71f);
    gGcc.update(auxCmd, 0, enable);
    gGcc.wait(auxCmd, enable, 0);
    tlmm.update(0, gpioConfig, gpioBefore & gpioConfig);
    tcsr.update(refGate, enable, refBefore & enable);
    gGcc.update(gdsc, enable | retainRegisters, gdscBefore & (enable | retainRegisters));
    return false;
}

class DpAux {
    const Registers &dptx;
    static constexpr unsigned kIntr = 0x20, kData = 0x234, kTransaction = 0x238;
    static constexpr unsigned kPhyIrqClear = 0x24c, kPhyIrqStatus = 0x2bc;
    static constexpr uint32_t kDone = 1u << 3, kNackDefer = 1u << 12, kI2cDefer = 1u << 21;
    static constexpr uint32_t kEvents = 0x09249248, kIndexWrite = 1u << 31, kGo = 1u << 9;

    unsigned transfer(uint32_t address, uint8_t *output, const uint8_t *input, unsigned size, uint32_t flags) {
        // msm_dp_aux_write packs address, direction and zero-based length.
        const uint8_t header[] = {static_cast<uint8_t>((address >> 16) | (output ? 0x10 : 0)),
                                  static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address),
                                  static_cast<uint8_t>(size - 1)};
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            dptx.write(kTransaction, 0);
            clear();
            for (unsigned i = 0; i < sizeof header; ++i)
                dptx.write(kData, (i ? 0 : kIndexWrite) | (uint32_t(header[i]) << 8));
            if (input)
                for (unsigned i = 0; i < size; ++i) dptx.write(kData, uint32_t(input[i]) << 8);
            dptx.write(kTransaction, flags | kGo);
            status = 0;
            for (unsigned tick = 0; tick < 250; ++tick) {
                status = dptx.read(kIntr) & kEvents;
                if (status) break;
                IOSleep(1);
            }
            if (!(status & (kNackDefer | kI2cDefer))) break;
            IODelay(500);
        }
        dptx.write(kTransaction, 0);
        if (status != kDone) return 0;
        if (!output) return size;

        dptx.write(kData, kIndexWrite | 1);
        (void)dptx.read(kData); // AUX reply framing byte
        unsigned received = 0;
        while (received < size) {
            const uint32_t word = dptx.read(kData);
            if (((word >> 16) & 0xff) != received) break;
            output[received++] = (word >> 8) & 0xff;
        }
        return received;
    }

public:
    uint32_t status = 0;
    static constexpr uint32_t kI2c = 0x100, kNoAddress = 0x400, kNoStop = 0x800;

    explicit DpAux(const Registers &regs) : dptx(regs) {}
    uint32_t phyStatus() const {return dptx.read(kPhyIrqStatus);}
    bool timedOut() const {return (status & (1u << 9)) != 0;}
    unsigned videoFifoFaults() const {
        // Read-only SDE DPTX SST underflow / mainlink FIFO overflow.
        return ((dptx.read(0x24)>>28)&1) | ((dptx.read(0x28)>>11)&2);
    }
    bool waitVideoIdle(const Registers &link) {
        // msm_dp_ctrl_push_idle: acknowledge the previous completion before
        // requesting a new idle pattern, then poll the DPTX0 local ISR.
        constexpr unsigned intrStatus2 = 0x24, stateCtrl = 0x04;
        constexpr uint32_t idleSent = 1u << 3, pushIdle = 1u << 8;
        constexpr uint32_t masks = ((1u << 0) | idleSent | (1u << 6) | (1u << 9)) << 2;
        const uint32_t interruptMask = dptx.read(intrStatus2) & masks;
        dptx.write(intrStatus2, interruptMask | (idleSent << 1));
        link.write(stateCtrl, pushIdle);
        for (unsigned elapsed = 0; elapsed < 30; ++elapsed) {
            if (dptx.read(intrStatus2) & idleSent) {
                dptx.write(intrStatus2, interruptMask | (idleSent << 1));
                return true;
            }
            IOSleep(1);
        }
        return false;
    }

    bool resetTransmitter() {
        // msm_dp_ctrl_off pulses the controller reset while its clocks are
        // still running. Restore the AUX registers for a rate fallback.
        constexpr unsigned swReset = 0x10, auxCtrl = 0x230, timeout = 0x23c, limits = 0x240;
        constexpr uint32_t reset = 1;
        const uint32_t ctrl = dptx.read(auxCtrl), timeLimit = dptx.read(timeout), auxLimits = dptx.read(limits);
        const uint32_t saved = dptx.read(swReset);
        dptx.write(swReset, saved | reset);
        IOSleep(1);
        dptx.write(swReset, saved & ~reset);
        if (!dptx.wait(swReset, reset, 0)) return false;
        dptx.write(timeout, timeLimit);
        dptx.write(limits, auxLimits);
        dptx.write(auxCtrl, ctrl);
        clear();
        return true;
    }

    void clear() {
        const uint32_t raw = dptx.read(kIntr);
        dptx.write(kIntr, (raw & (kEvents << 2)) | ((raw & kEvents) << 1));
        (void)phyStatus();
        dptx.write(kPhyIrqClear, 0x1f);
        dptx.write(kPhyIrqClear, 0x9f);
        dptx.write(kPhyIrqClear, 0);
    }
    unsigned read(uint32_t address, uint8_t *data, unsigned size, uint32_t flags = 0) {
        return transfer(address, data, nullptr, size, flags);
    }
    bool write(uint32_t address, const uint8_t *data, unsigned size, uint32_t flags = 0) {
        return transfer(address, nullptr, data, size, flags) == size;
    }
};

bool readDpEdidBlock(DpAux &aux, unsigned number, uint8_t (&block)[128]) {
    // The Qualcomm EDID workaround sets the offset for each 16-byte chunk.
    constexpr uint32_t readFlags = DpAux::kI2c | DpAux::kNoAddress;
    constexpr uint32_t writeFlags = readFlags | DpAux::kNoStop;
    const uint8_t segment = number / 2;
    for (unsigned chunk = 0; chunk < sizeof block; chunk += 16) {
        const uint8_t offset = (number % 2) * 128 + chunk;
        if ((segment && !aux.write(0x30, &segment, 1, writeFlags)) ||
            !aux.write(0x50, &offset, 1, writeFlags) ||
            aux.read(0x50, block + chunk, 16, readFlags) != 16) {
            printf("[dp aux] EDID read failed block=%u offset=%u status=%08x\n", number, offset, aux.status);
            return false;
        }
    }
    uint8_t sum = 0;
    for (uint8_t byte : block) sum += byte;
    constexpr uint8_t header[] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    if (sum || (number == 0 && memcmp(block, header, sizeof header))) {
        printf("[dp aux] EDID invalid block=%u checksum=%02x\n", number, sum);
        return false;
    }
    return true;
}

class DpSinkCrc {
    DpAux &aux;
    static constexpr unsigned kSinkStatus = 0x202, kCrc = 0x240, kCrcMisc = 0x246, kTest = 0x270;
    uint8_t original = 0;
    bool enabled = false, owned = false;
public:
    explicit DpSinkCrc(DpAux &channel) : aux(channel) {}
    void start() {
        uint8_t misc = 0;
        if (aux.read(kCrcMisc, &misc, 1) != 1 || !(misc & 0x20)) return;
        if (aux.read(kTest, &original, 1) != 1) return;
        const uint8_t start = original | 1u;
        owned = start != original && aux.write(kTest, &start, 1);
        enabled = (original & 1u) || owned;
    }
    void sample(const char *phase) {
        UkeDpLink::Status status;
        const bool synced = aux.read(kSinkStatus, status.bytes, sizeof status.bytes) == sizeof status.bytes;
        uint8_t crc[7]{};
        const bool valid = enabled && aux.read(kCrc, crc, sizeof crc) == sizeof crc;
        printf("[dp sink] %s status=%u lanes=%02x align=%02x port=%02x crc=%u count=%u rgb=%04x/%04x/%04x\n",
               phase, synced, status.bytes[0], status.bytes[2], status.bytes[3], valid, crc[6] & 15,
               crc[0] | crc[1] << 8, crc[2] | crc[3] << 8, crc[4] | crc[5] << 8);
    }
    void symbolErrors() {
        uint8_t counts[4]{};
        const unsigned bytes=aux.read(0x210,counts,sizeof counts);
        printf("[dp sink] symbol errors bytes=%u lane0=%04x lane1=%04x\n",bytes,
               counts[0] | counts[1] << 8,counts[2] | counts[3] << 8);
    }
    void stop() {if (owned) (void)aux.write(kTest, &original, 1);}
};

struct DpDisplayInfo {
    UkeDpVideo::ModeSelection modes;
    UkeDpVideo::Mode preferred{};
    uint8_t edid[3][128]{};
    unsigned length=0;
    bool desktop=false;
};

void readDpEdid(DpAux &aux, DpDisplayInfo *info) {
    uint8_t block[128]={};
    if (!readDpEdidBlock(aux, 0, block)) return;
    const auto *timing = block + 54;
    const unsigned pixelKHz = (timing[0] | unsigned(timing[1]) << 8) * 10;
    if (native_legacy_dp_debug()) printf("[dp aux] EDID vendor=%02x%02x product=%02x%02x first=%ux%u pixel_khz=%u extensions=%u\n",
           block[8], block[9], block[11], block[10], timing[2] | (timing[4] & 0xf0u) << 4,
           timing[5] | (timing[7] & 0xf0u) << 4, pixelKHz, block[126]);
    if (!info) return;
    (void)UkeDpVideo::detailedTiming(timing,info->preferred);
    const unsigned extensions = block[126] > 2 ? 2 : block[126];
    if (!info->desktop) info->modes.base(block);
    memcpy(info->edid[0],block,sizeof block);
    info->length=sizeof block;
    for (unsigned index = 1; index <= extensions; ++index)
        if (readDpEdidBlock(aux, index, block)) {
            if (!info->desktop) info->modes.cta(block);
            if (info->length==index*sizeof block) {
                memcpy(info->edid[index],block,sizeof block);
                info->length+=sizeof block;
            }
        }
    if (info->length != (unsigned(info->edid[0][126])+1)*sizeof block) {
        // Publish a self-consistent EDID if a later block was unavailable.
        auto *base=info->edid[0];
        const unsigned count=info->length/sizeof block-1;
        base[127]+=base[126]-count;
        base[126]=count;
    }
    if (extensions > 1) {
        const uint8_t segmentZero = 0;
        (void)aux.write(0x30, &segmentZero, 1, DpAux::kI2c | DpAux::kNoAddress);
    }
    if (!info->desktop)
        if (native_legacy_dp_debug()) printf("[dp video] EDID eligible modes HBR2=%ux%u@%u kHz HBR=%ux%u@%u kHz RBR=%ux%u@%u kHz\n",
               info->modes.hbr2.width, info->modes.hbr2.height, info->modes.hbr2.pixel,
               info->modes.hbr.width, info->modes.hbr.height, info->modes.hbr.pixel,
               info->modes.rbr.width, info->modes.rbr.height, info->modes.rbr.pixel);
}

constexpr unsigned kDpPhy = 0x2a00, kDpSerdes = 0x2000;
constexpr unsigned kDpTxBanks[] = {0x2200, 0x2600};
constexpr unsigned kDpPhyPower = kDpPhy + 0x18;

void initDpAuxPhy(const Registers &phy) {
    // Qualcomm qmp_v4_dp_aux_init on the Uke v6 QSERDES bank layout.
    constexpr uint8_t auxCfg[] = {0, 0x13, 0xa4, 0, 0x0a, 0x26, 0x0a, 3, 0xb7, 3};
    phy.write(kDpPhyPower, 0x67);
    phy.write(kDpSerdes + 0xdc, 0x17);
    for (unsigned i = 0; i < sizeof auxCfg; ++i) phy.write(kDpPhy + 0x20 + i * 4, auxCfg[i]);
    phy.write(kDpPhy + 0x54, 0x1f);
}

void setDpLevels(const Registers &phy, uint8_t rate, UkeDpLink::SignalLevels levels) {
    const auto &swing = rate == UkeDpLink::kHbr2 ? kUkeDpHbr2Swing : kUkeDpSwing;
    const auto &pre = rate == UkeDpLink::kHbr2 ? kUkeDpHbr2PreEmphasis : kUkeDpPreEmphasis;
    for (unsigned base : kDpTxBanks) {
        phy.write(base + 0x14, swing[levels.swing][levels.pre] | 0x20);
        phy.write(base + 0x0c, pre[levels.swing][levels.pre] | 0x20);
    }
}

bool startDpPhy(const Registers &phy, bool reverse, uint8_t rate) {
    constexpr unsigned config = kDpPhy + 0x10, status = kDpPhy + 0xe4;
    // OEM v420 PHY shares orientation (CC1=1/CC2=2) and active lanes with PLL.
    phy.write(kDpPhy + 0xc8, ((reverse ? 2u : 1u) << 4) | 2u);
    const auto &rateTable = rate == UkeDpLink::kHbr2 ? kUkeDpHbr2 :
                           rate == UkeDpLink::kHbr ? kUkeDpHbr : kUkeDpRbr;
    for (const auto &entry : kUkeDpSerdes) phy.write(kDpSerdes + entry.offset, entry.value);
    for (const auto &entry : rateTable) phy.write(kDpSerdes + entry.offset, entry.value);
    for (unsigned base : kDpTxBanks)
        for (const auto &entry : kUkeDpTx) phy.write(base + entry.offset, entry.value);
    setDpLevels(phy, rate, {});

    // qmp_v456_configure_dp_phy followed by the v4 two-lane TX setup.
    phy.write(kDpPhy + 0x14, 0x0f);
    phy.write(kDpPhyPower, 0x65 | (reverse ? 0x08 : 0x10));
    phy.write(kDpPhy + 0x1c, reverse ? 0x4c : 0x5c);
    phy.write(kDpPhy + 0x24, 0x13);
    phy.write(kDpPhy + 0x28, 0xa4);
    phy.write(kDpPhy + 0x78, 0x05);
    phy.write(kDpPhy + 0x9c, 0x05);
    phy.write(kDpPhy + 0x70, ukeDpPhyVcoDividerCode(rate));
    constexpr uint8_t sequence[] = {0x01, 0x05, 0x01, 0x09};
    for (uint8_t value : sequence) phy.write(config, value);
    phy.write(kDpSerdes + 0x118, 0x20);
    if (!phy.wait(kDpSerdes + 0x1f8, 1, 1, 1000) ||
        !phy.wait(kDpSerdes + 0x1d0, 3, 3, 1000)) {
        printf("[dp train] PLL wait failed rate=%02x ready=%08x cmn=%08x\n",
               rate, phy.read(kDpSerdes + 0x1f8), phy.read(kDpSerdes + 0x1d0));
        return false;
    }
    phy.write(config, 0x19);
    if (!phy.wait(status, 3, 3, 1000)) {
        printf("[dp train] DP PHY ready wait failed rate=%02x status=%08x\n", rate, phy.read(status));
        return false;
    }
    phy.write(kDpTxBanks[0] + 0x58, 0x10);
    phy.write(kDpTxBanks[1] + 0x58, 0x10);
    phy.write(kDpTxBanks[0] + 0x54, reverse ? 0x3f : 0x15);
    phy.write(kDpTxBanks[1] + 0x54, reverse ? 0x15 : 0x3f);
    phy.write(config, 0x18);
    IODelay(2000);
    phy.write(config, 0x19);
    if (!phy.wait(status, 3, 3, 1000)) {
        printf("[dp train] DP TX sync wait failed rate=%02x status=%08x\n", rate, phy.read(status));
        return false;
    }
    for (unsigned base : kDpTxBanks) phy.write(base + 0x5c, 0x0a);
    setDpLevels(phy, rate, {});
    if (native_legacy_dp_debug()) printf("[dp phy] native rate=%02x spare=%08x ivco=%08x cmn=%08x status=%08x\n",
           rate,phy.read(kDpPhy+0xc8),phy.read(kDpSerdes+0xf4),phy.read(kDpSerdes+0x1d0),phy.read(status));
    return true;
}

class DpLinkClocks {
    const Registers &disp;
    static constexpr unsigned kRcg = 0x8170, kConfig = 0x8174;
    static constexpr unsigned kLink = 0x8040, kInterface = 0x8048;
    static constexpr uint32_t kOn = 1, kHalted = 1u << 31, kRcgMask = 0x371f;
    uint32_t savedConfig = 0;
    bool owned = false;
public:
    explicit DpLinkClocks(const Registers &regs) : disp(regs) {}
    bool start() {
        if ((disp.read(kLink) | disp.read(kInterface)) & kOn) return false;
        savedConfig = disp.read(kConfig);
        owned = true;
        disp.update(kConfig, kRcgMask, 0x101); // DP0 PHY PLL link clock, /1
        disp.update(kRcg, 0, kOn);
        if (!disp.wait(kRcg, kOn, 0)) return false;
        disp.update(kLink, kOn, kOn);
        disp.update(kInterface, kOn, kOn);
        return disp.wait(kLink, kHalted | kOn, kOn) && disp.wait(kInterface, kHalted | kOn, kOn);
    }
    ~DpLinkClocks() {
        if (!owned) return;
        disp.update(kInterface, kOn, 0);
        disp.update(kLink, kOn, 0);
        disp.update(kConfig, kRcgMask, savedConfig & kRcgMask);
        disp.update(kRcg, 0, kOn);
        if (!disp.wait(kRcg, kOn, 0)) printf("[dp train] link RCG restore timed out\n");
    }
};

class DpPixelClock {
    const Registers &disp;
    static constexpr unsigned kBranch = 0x8050, kCmd = 0x818c, kCfg = kCmd + 4;
    static constexpr unsigned kM = kCmd + 8, kN = kCmd + 12, kD = kCmd + 16;
    static constexpr uint32_t kCfgMask = 0x10371f;
    uint32_t saved[4]{};
    bool owned = false;
public:
    explicit DpPixelClock(const Registers &regs) : disp(regs) {}
    unsigned m = 0, n = 0;
    bool start(unsigned pixel, uint8_t rate) {
        if (disp.read(kBranch) & 1) return false;
        const auto ratio = UkeDpVideo::pixelRatio(pixel, rate);
        m = ratio.m;
        n = ratio.n;
        if (!m || 2 * m > n) return false;
        saved[0] = disp.read(kCfg);
        saved[1] = disp.read(kM);
        saved[2] = disp.read(kN);
        saved[3] = disp.read(kD);
        owned = true;
        disp.update(kM, 0xffff, m);
        disp.update(kN, 0xffff, ~(n - m) & 0xffff);
        disp.update(kD, 0xffff, ~n & 0xffff);
        disp.update(kCfg, kCfgMask, 0x2201); // DP0 VCO divider, /1, dual-edge M/N.
        disp.update(kCmd, 0, 1);
        if (!disp.wait(kCmd, 1, 0)) return false;
        disp.update(kBranch, 1, 1);
        return disp.wait(kBranch, 0x80000001u, 1);
    }
    ~DpPixelClock() {
        if (!owned) return;
        disp.update(kBranch, 1, 0);
        disp.update(kM, 0xffff, saved[1] & 0xffff);
        disp.update(kN, 0xffff, saved[2] & 0xffff);
        disp.update(kD, 0xffff, saved[3] & 0xffff);
        disp.update(kCfg, kCfgMask, saved[0] & kCfgMask);
        disp.update(kCmd, 0, 1);
        if (!disp.wait(kCmd, 1, 0)) printf("[dp video] pixel RCG restore timed out\n");
    }
};

// Both slots stay wired for the connector session. They belong to the
// external framebuffer, with the built-in display retaining its own slots.
// The role thread owns the table until a DPU output has a framebuffer client.
class DpExternalBuffers {
    static constexpr unsigned kSlotCount = 2, kPageBytes = 4096;
    static bool blocked;
    struct Slot {
        IOBufferMemoryDescriptor *memory = nullptr;
        uint32_t iova = 0;
        bool prepared = false, mappingAttempted = false;
    };
    Slot slots[kSlotCount];
    unsigned pitch = 0;

public:
    bool prepare(const UkeDpVideo::Mode &mode) {
        pitch = (mode.width * 4 + 63) & ~63u;
        const uint64_t visible = uint64_t(pitch) * mode.height;
        if (blocked || !&uke_ufs_dpu_map_external || !visible || visible > (32u << 20)) return false;
        const unsigned pagesCount = (visible + kPageBytes - 1) / kPageBytes;
        auto *pages = static_cast<uint64_t *>(IOMallocData(pagesCount * sizeof(uint64_t)));
        if (!pages) return false;
        bool ready = true;
        for (unsigned index = 0; index < kSlotCount && ready; ++index) {
            auto &slot = slots[index];
            const auto length = pagesCount * kPageBytes;
            slot.memory = IOBufferMemoryDescriptor::inTaskWithOptions(
                kernel_task, kIODirectionInOut, length, kPageBytes);
            if (!slot.memory || !slot.memory->getBytesNoCopy() || slot.memory->prepare() != kIOReturnSuccess) {
                ready = false;
                break;
            }
            slot.prepared = true;
            for (unsigned page = 0; page < pagesCount; ++page) {
                IOByteCount segment = 0;
                pages[page] = slot.memory->getPhysicalSegment(page * kPageBytes, &segment, kIOMemoryMapperNone);
                if (!pages[page] || (pages[page] & (kPageBytes - 1)) || segment < kPageBytes) {
                    ready = false;
                    break;
                }
            }
            if (!ready) break;
            slot.mappingAttempted = true;
            slot.iova = uke_ufs_dpu_map_external(index, pages, pagesCount);
            ready = slot.iova != 0;
        }
        IOFreeData(pages, pagesCount * sizeof(uint64_t));
        if (ready && native_legacy_dp_debug()) printf("[dp ext] buffers %ux%u stride=%u iova=%08x/%08x\n",
                          mode.width, mode.height, pitch, slots[0].iova, slots[1].iova);
        return ready;
    }

    uint32_t address(unsigned slot) const {return slots[slot].iova;}
    unsigned stride() const {return pitch;}
    void *pixels(unsigned slot) const {return slots[slot].memory->getBytesNoCopy();}
    unsigned bytes(unsigned slot) const {return unsigned(slots[slot].memory->getLength());}
    void clean(unsigned slot, unsigned height) const {
        const uintptr_t base=reinterpret_cast<uintptr_t>(pixels(slot));
        const uintptr_t end=base+size_t(pitch)*height;
        for(uintptr_t addr=base;addr<end;addr+=64) __asm__ volatile("dc cvac, %0" :: "r"(addr) : "memory");
        sync();
    }
    void paint(const UkeDpVideo::Mode &mode, bool desktop) const {
        constexpr uint32_t colors[2][4] = {
            {0xffdc2020, 0xff20b040, 0xff2040dc, 0xffdddddd},
            {0xff20c4c4, 0xffbb32bb, 0xffd0b020, 0xff303030},
        };
        for (unsigned slot = 0; slot < kSlotCount; ++slot) {
            auto *row = static_cast<uint32_t *>(pixels(slot));
            for (unsigned x = 0; x < mode.width; ++x) row[x] = desktop ? 0xff000000 : colors[slot][x * 4 / mode.width];
            for (unsigned x = mode.width; x < pitch / 4; ++x) row[x] = 0;
            for (unsigned y = 1; y < mode.height; ++y) memcpy(row + y * (pitch / 4), row, pitch);
            clean(slot,mode.height);
        }
    }

    void retainWhileBusy() {
        blocked = true;
        // The dedicated CTL did not retire. Preserve its IOVA mappings and
        // keep the descriptor references and wires alive for this boot.
        for (auto &slot : slots) slot = {};
    }

    ~DpExternalBuffers() {
        for (unsigned index = 0; index < kSlotCount; ++index) {
            auto &slot = slots[index];
            if (slot.mappingAttempted && !uke_ufs_dpu_map_external(index, nullptr, 0)) {
                // A stale translation may still reach this buffer. Keep the
                // pages wired and refuse any further allocations this boot.
                blocked = true;
                printf("[dp ext] slot %u invalidation failed, retaining backing pages\n", index);
                continue;
            }
            if (slot.prepared) slot.memory->complete();
            OSSafeReleaseNULL(slot.memory);
        }
    }
};
bool DpExternalBuffers::blocked;

class DpuDpScanout {
    Registers mdp, vbif;
    UkeDpDpu::Write saved[120]{};
    unsigned count = 0;
    uint32_t oldQos[2][8]{}, oldLimit = 0, oldClock = 0;
    uint32_t oldFifoSize = 0;
    bool fifoProgrammed = false;
    NativeExternalScanoutRect output{};
    uint32_t outputSize=0,outputOrigin=0,inputSize=0;
    unsigned pipe=UkeDpDpu::kDma,xin=UkeDpDpu::kXin;
    uint32_t pipeFlush=UkeDpDpu::kDmaFlush;
    bool xr10Pipe=false,inputXr10=false;

    uint32_t flipMask() const {return pipeFlush|UkeDpDpu::kDpPeriphFlush;}
    uint32_t initialMask() const {return (UkeDpDpu::kInitialFlush&~UkeDpDpu::kDmaFlush)|pipeFlush;}
    uint32_t limitShift() const {return (xin%4)*8;}
    uint32_t limitReg() const {return UkeDpDpu::kVbifReadLimit+(xin/4)*4;}
    uint32_t qosMask() const {return 7u<<((xin%8)*4);}

    void saveWrite(unsigned offset,uint32_t value) {
        saved[count++]={offset,mdp.read(offset)};
        mdp.write(offset,value);
    }

    void prepareXr10Pipe() {
        using namespace UkeDpDpu;
        // The XR10 color plane has the XRGB2101010 bit layout. Translate
        // its RGB black/white codes in the VIG CSC with YUV select clear.
        const Write color[]={
            {kCsc10,0}, {kCsc10+0x004,kVigXr10Scale},
            {kCsc10+0x008,0}, {kCsc10+0x00c,kVigXr10Scale},
            {kCsc10+0x010,0}, {kCsc10+0x014,kVigXr10Scale},
            {kCsc10+0x018,0x03ff}, {kCsc10+0x01c,0x03ff}, {kCsc10+0x020,0x03ff},
            {kCsc10+0x024,0x03ff}, {kCsc10+0x028,0x03ff}, {kCsc10+0x02c,0x03ff},
            {kCsc10+0x030,kVigXr10Bias}, {kCsc10+0x034,kVigXr10Bias}, {kCsc10+0x038,kVigXr10Bias},
            {kCsc10+0x03c,0}, {kCsc10+0x040,0}, {kCsc10+0x044,0},
        };
        for(const auto &reg:color)saveWrite(pipe+reg.offset,reg.value);
        constexpr unsigned scalerOffsets[]={0x004,0x010,0x014,0x018,0x01c,0x020,
            0x040,0x044,0x048,0x090,0x094,0x098,0x09c};
        for(unsigned offset:scalerOffsets)
            saveWrite(pipe+kQseed+offset,offset==0x004?1u<<14:0u);
    }

    void setXr10Scaler(bool xr10,uint32_t source,uint32_t destination) {
        using namespace UkeDpDpu;
        const unsigned scale=pipe+kQseed;
        if(!xr10 || source==destination) {
            mdp.write(scale+0x004,xr10?0u:1u<<14);
            return;
        }
        const uint32_t h=qseedPhase(source&0xffff,destination&0xffff);
        const uint32_t v=qseedPhase(source>>16,destination>>16);
        mdp.write(scale+0x010,h);mdp.write(scale+0x014,v);
        mdp.write(scale+0x018,h);mdp.write(scale+0x01c,v);
        mdp.write(scale+0x020,kCliffsQseedPreload);
        mdp.write(scale+0x040,source);mdp.write(scale+0x044,source);
        mdp.write(scale+0x048,destination);
        // Linux and OEM default bilinear configuration: zero initial phase,
        // 21-bit increments, RGB and UV share geometry, and no LUT upload.
        mdp.write(scale+0x004,kQseedBilinearRgb);
    }

    bool prepareFetch() {
        using namespace UkeDpDpu;
        const uint32_t xinBit = 1u << xin;
        const uint32_t version = vbif.read(0);
        const uint32_t halt = vbif.read(kVbifHalt);
        if (!version || version == ~0u || (halt & xinBit)) {
            printf("[dp dpu] VBIF unavailable version=%08x halt=%08x\n", version, halt);
            return false;
        }
        oldClock = mdp.read(pipe + 0x330);
        mdp.update(pipe + 0x330, 1, 1);

        oldLimit = vbif.read(limitReg()) & (0xffu<<limitShift());
        if (oldLimit != (0x28u<<limitShift())) {
            // sde_vbif_set_ot_limit: halt only the fetch client being changed.
            vbif.update(limitReg(),0xffu<<limitShift(),0x28u<<limitShift());
            vbif.update(kVbifHalt, xinBit, xinBit);
            const bool halted = vbif.wait(kVbifHalt + 4, xinBit << 16, xinBit << 16, 400);
            vbif.update(kVbifHalt, xinBit, 0);
            if (!halted) {
                vbif.update(limitReg(),0xffu<<limitShift(),oldLimit);
                mdp.update(pipe + 0x330, 1, oldClock & 1);
                printf("[dp dpu] VBIF XIN%u halt ack timed out status=%08x\n",xin,vbif.read(kVbifHalt + 4));
                return false;
            }
        }
        constexpr unsigned bases[] = {kVbifQosRequest, kVbifQosLevel};
        for (unsigned table = 0; table < 2; ++table) {
            for (unsigned priority = 0; priority < sizeof kRtPriority; ++priority) {
                const unsigned reg = xinQosOffset(bases[table], priority);
                oldQos[table][priority] = vbif.read(reg) & qosMask();
                vbif.update(reg, qosMask(), xinQosValue(priority,xin));
            }
        }
        if (native_legacy_dp_debug()) printf("[dp dpu] VBIF XIN%u version=%08x readLimit=%02x->%02x clock=%08x/%08x\n",
               xin,version,oldLimit>>limitShift(),(vbif.read(limitReg()) & (0xffu<<limitShift()))>>limitShift(),
               mdp.read(pipe + 0x330),mdp.read(pipe + 0x334));
        return true;
    }

    void restoreFetch() {
        using namespace UkeDpDpu;
        constexpr unsigned bases[] = {kVbifQosRequest, kVbifQosLevel};
        for (unsigned table = 0; table < 2; ++table)
            for (unsigned priority = 0; priority < sizeof kRtPriority; ++priority)
                vbif.update(xinQosOffset(bases[table], priority), qosMask(), oldQos[table][priority]);
        vbif.update(limitReg(),0xffu<<limitShift(),oldLimit);
        mdp.update(pipe + 0x330, 1, oldClock & 1);
    }

public:
    bool start(const UkeDpVideo::Mode &mode, const DpExternalBuffers &buffers,bool xr10=false) {
        using namespace UkeDpDpu;
        if (!mdp.map(0x0ae01000, 0x50000) || !vbif.map(0x0aeb0000, 0x2008)) return false;
        const uint32_t revision = mdp.read(0), major = revision >> 28;
        if (major != 9 && major != 10) {
            printf("[dp dpu] unsupported MDP revision=%08x\n", revision);
            return false;
        }
        if (const char *busy = busyResource(mdp)) {
            printf("[dp dpu] cannot reserve DMA0/LM2/PP2/CTL2/INTF0, busy=%s\n", busy);
            return false;
        }
        if(xr10) {
            if(revision!=0xa0010000 || !native_legacy_panel_dma_left_ready() || !vig0Available(mdp) ||
               mode.width!=2560 || mode.height!=1440) {
                printf("[dp xr10] VIG0 ownership or QHD mode rejected\n");
                return false;
            }
            pipe=kVig0;
            xin=kVig0Xin;
            pipeFlush=kVig0Flush;
            xr10Pipe=true;
        }
        const uint32_t fsr = &native_ufs_dpu_live ? native_ufs_dpu_live(1) : ~0u;
        if (fsr & 0xc00001feU) {
            printf("[dp dpu] display DMA context unavailable fsr=%08x\n", fsr);
            return false;
        }
        if (!prepareFetch()) return false;
        output={mode.width,mode.height,0,0};
        if (revision >> 16 == 0xa001) { // Cliffs catalog programs PPB through MDP top.
            const uint32_t before = mdp.read(kPp2Fifo);
            oldFifoSize = before & kPp2FifoMask;
            mdp.update(kPp2Fifo, kPp2FifoMask, pp2FifoSize(mode.width));
            fifoProgrammed = true;
            if (native_legacy_dp_debug()) printf("[dp dpu] PP2 FIFO %08x->%08x\n", before, mdp.read(kPp2Fifo));
        }
        const uint32_t dimensions = mode.height << 16 | mode.width;
        outputSize=inputSize=dimensions;
        const auto video = timing(mode,true);
        // dpu_hw_sspp: linear ARGB8888 (little endian BGRA), solo rect.
        // dpu_hw_ctl: stage BASE for DMA0 on LM2 and active INTF0.
        const Write rows[] = {
            {pipe + 0x000, dimensions}, {pipe + 0x008, 0}, {pipe + 0x00c, dimensions}, {pipe + 0x010, 0},
            {pipe + 0x014, buffers.address(0)}, {pipe + 0x018, 0}, {pipe + 0x01c, 0}, {pipe + 0x020, 0},
            {pipe + 0x024, buffers.stride()}, {pipe + 0x028, 0},
            {pipe + 0x030, 0x000237ff}, {pipe + 0x034, 0x03020001}, {pipe + 0x038, 1u << 31},
            {pipe + 0x040, 0}, {pipe + 0x0b4, 0}, {pipe + 0x134, 0},
            // OEM real-time linear DMA0 QoS and non-secure solo addresses.
            {pipe + 0x060, 0xffff}, {pipe + 0x064, 0xfff0}, {pipe + 0x06c, 1},
            {pipe + 0x070, mdp.read(pipe + 0x070) & ~0xfu},
            {pipe + 0x074, 0x44556666}, {pipe + 0x078, 0x00112233},
            // OEM sde_plane sets pixel extension even for unscaled DMA
            // planes. The requested region is the full source for RGB.
            {pipe + 0x100, 0}, {pipe + 0x104, 0}, {pipe + 0x108, dimensions},
            {pipe + 0x110, 0}, {pipe + 0x114, 0}, {pipe + 0x118, dimensions},
            {pipe + 0x120, 0}, {pipe + 0x124, 0}, {pipe + 0x128, dimensions},
            {pipe + 0x170, 0},
            {kMixer + 0x000, 0}, {kMixer + 0x004, dimensions},
            {kMixer + 0x008, 0}, {kMixer + 0x010, 0},
            // The first blend stage is opaque when DMA0 uses a cropped
            // rectangle. Cliffs uses the combined FG/BG alpha register.
            {kMixer + 0x020, 1u << 8}, {kMixer + 0x024, 0xffu << 16},
            {kCtl + 0x008, xr10Pipe?kVig0BaseStage:kDmaBaseStage}, {kCtl + 0x048, 0},
            {kCtl + 0x078, 0}, {kCtl + 0x0a8, 0}, {kCtl + 0x0c0, 0},
            {kCtl + 0x014, 0xf0000000}, {kCtl + 0x0f4, 1},
            {kCtl + 0x0fc, xr10Pipe?kVig0Fetch:1u}, {kCtl + 0x100, kMerge1Flush}, {kCtl + 0x110, 1},
            {kPeriphFlushReg, 1}, {kCtl + 0x134, 1},
            {kMerge + 0x004, 0}, // PP2 uses the unmerged, single-mixer stream.
            {kIntf + 0x004, video.config},
            {kIntf + 0x008, video.hsync}, {kIntf + 0x00c, video.vperiod}, {kIntf + 0x014, video.vwidth},
            {kIntf + 0x01c, video.vstart}, {kIntf + 0x024, video.vend},
            {kIntf + 0x02c, video.vstart}, {kIntf + 0x034, video.vaend},
            {kIntf + 0x03c, video.hactive}, {kIntf + 0x040, video.hactive},
            {kIntf + 0x044, 0}, {kIntf + 0x048, 0}, {kIntf + 0x04c, 0},
            {kIntf + 0x050, (unsigned(mode.vlow) << 1) | unsigned(mode.hlow)},
            {kIntf + 0x060, 0x11}, {kIntf + 0x064, video.hactive}, {kIntf + 0x068, 0},
            {kIntf + 0x090, 0x213f}, {kIntf + 0x0a8, 3},
            {kIntf + 0x100, 0}, {kIntf + 0x170, video.prefetch},
            {kIntf + 0x25c, (mdp.read(kIntf + 0x25c) & ~0xfu) | 2u},
        };
        static_assert(sizeof(rows)+(31+2)*sizeof(saved[0]) <= sizeof(saved));
        for (const auto &row : rows) {
            saveWrite(row.offset,row.value);
        }
        if(xr10Pipe)prepareXr10Pipe();
        const auto armMisr = [&](unsigned reg, uint32_t dataSelect) {
            saved[count++] = {reg, mdp.read(reg) & ~((1u << 9) | (1u << 10))};
            mdp.write(reg, 1u << 10); // Clear the previous hardware signature.
            mdp.write(reg, 0x80000101u | dataSelect);
        };
        if (native_legacy_dp_debug()) {
            armMisr(kMixer + 0x310, 0);
            armMisr(kIntf + 0x180, 1u << 24);
        }
        // OEM holds the override only while updating the VBIF client.
        mdp.update(pipe + 0x330, 1, oldClock & 1);
        if (native_legacy_dp_debug()) printf("[dp dpu] armed %s/LM2/PP2/CTL2/INTF0 rev=%08x\n",xr10Pipe?"VIG0":"DMA0",revision);
        return true;
    }

    void enable() {
        using namespace UkeDpDpu;
        // The DP peripheral registers and source timing must precede CTL's
        // initial commit. The OEM video encoder then enables the INTF.
        mdp.write(kFlush,initialMask());
        mdp.write(kTiming,1);
        if (native_legacy_dp_debug()) printf("[dp dpu] native commit periph=%08x pending=%08x\n",mdp.read(kPeriphFlushReg),mdp.read(kFlush));
    }
    uint32_t frame() const {return mdp.read(UkeDpDpu::kFrame);}
    bool initialCommitRetired() const {return !(mdp.read(UkeDpDpu::kFlush)&initialMask());}
    void sample(const Registers &link, const DpAux &aux, const char *phase) const {
        using namespace UkeDpDpu;
        printf("[dp dpu] %s frame=%u ctl=%08x src=%08x req=%08x underrun=%u misr=%08x/%08x link=%08x fifo=%u fsr=%08x\n",
               phase,frame(),mdp.read(kFlush),mdp.read(pipe+0x014),mdp.read(pipe+0x108),
               mdp.read(kIntf+0x268),mdp.read(kIntf+0x180),mdp.read(kIntf+0x184),link.read(0x40),
               aux.videoFifoFaults(),native_ufs_dpu_live(1));
        printf("[dp dpu] %s pixels lm=%08x/%08x dp=%08x/%08x/%08x stage=%08x fetch=%08x pp=%08x dma=%08x vbif=%08x/%08x panel_frames=%u/%u\n",
               phase, mdp.read(kMixer + 0x310), mdp.read(kMixer + 0x314),
               link.read(0xd0), link.read(0xd4), link.read(0xdc),
               mdp.read(kCtl + 0x008), mdp.read(kCtl + 0x0fc), mdp.read(kIntf + 0x25c),
               mdp.read(pipe + 0x334), vbif.read(0x190), vbif.read(0x194), mdp.read(0x350ac), mdp.read(0x360ac));
        if (mdp.read(kTiming) & 1) link.write(0xd0, 0x501); // Sample the next live frame.
    }
    bool healthy() const {
        return (mdp.read(UkeDpDpu::kIntfStatus) & 1) && &native_ufs_dpu_live &&
               !(native_ufs_dpu_live(1) & 0xc00001feU);
    }
    bool queueFlip(uint32_t iova, uint32_t &before, unsigned pitch=0,
                   const NativeExternalScanoutRect *target=nullptr,
                   bool xr10=false,unsigned sourceWidth=0,unsigned sourceHeight=0) {
        using namespace UkeDpDpu;
        if (mdp.read(kFlush) & initialMask()) return false;
        before = frame();
        mdp.write(pipe+0x014,iova);
        if(pitch)mdp.write(pipe+0x024,pitch);
        const auto &rect=target?*target:output;
        const uint32_t size=rect.height<<16 | rect.width,origin=rect.y<<16 | rect.x;
        const uint32_t sourceSize=xr10?(sourceHeight<<16 | sourceWidth):size;
        uint32_t flush=flipMask();
        if(size!=outputSize || origin!=outputOrigin || sourceSize!=inputSize || xr10!=inputXr10) {
            mdp.write(pipe+0x000,sourceSize);
            mdp.write(pipe+0x00c,size);
            mdp.write(pipe+0x010,origin);
            mdp.write(pipe+0x108,sourceSize);
            mdp.write(pipe+0x118,sourceSize);
            mdp.write(pipe+0x128,sourceSize);
            const bool fillsOutput=!origin && rect.width==output.width && rect.height==output.height;
            if(xr10Pipe) {
                mdp.write(pipe+0x030,xr10?kXrgb2101010:kArgb8888);
                mdp.write(pipe+kCsc10,xr10?1u:0u); // RGB is selected by leaving bit 1 clear.
                setXr10Scaler(xr10,sourceSize,size);
                mdp.write(kCtl+0x008,fillsOutput?kVig0BaseStage:kVig0OverBorder);
            } else {
                mdp.write(kCtl+0x008,fillsOutput?kDmaBaseStage:kDmaOverBorder);
            }
            inputSize=sourceSize;
            inputXr10=xr10;
            outputSize=size;
            outputOrigin=origin;
            flush|=kMixerCommit;
        }
        mdp.write(kPeriphFlushReg,1);
        mdp.write(kFlush,flush);
        return true;
    }
    bool flipLatched(uint32_t before) const {
        using namespace UkeDpDpu;
        return frame() != before && !(mdp.read(kFlush) & (flipMask()|kMixerCommit));
    }
    bool flip(uint32_t iova) {
        uint32_t before;
        if (!queueFlip(iova, before)) return false;
        for (unsigned tick = 0; tick < 80; ++tick) {
            if (flipLatched(before)) return healthy();
            IOSleep(1);
        }
        return false;
    }
    bool stop() {
        using namespace UkeDpDpu;
        mdp.write(kTiming, 0);
        for (unsigned tick = 0; tick < 80 && (mdp.read(kIntfStatus) & 1); ++tick) IOSleep(1);
        if (mdp.read(kIntfStatus) & 1) return false;
        mdp.write(kReset, 1);
        if (!mdp.wait(kReset, 1, 0)) return false;
        while (count) {
            const auto &row = saved[--count];
            mdp.write(row.offset, row.value);
        }
        if (fifoProgrammed) mdp.update(kPp2Fifo, kPp2FifoMask, oldFifoSize);
        mdp.update(pipe + 0x330, 1, 1);
        restoreFetch();
        return true;
    }
};

struct DpDesktopScanout {
    DpExternalBuffers &buffers;
    DpuDpScanout &dpu;
    unsigned width,height;
    unsigned shown=0;
    bool faulted=false;
    unsigned submitted=0;
    uint32_t pendingFrame=0;
    uint64_t pendingDeadline=0;
    struct DirectSlot {IOMemoryDescriptor *memory=nullptr;uint32_t iova=0;uint64_t used=0,mappedBytes=0;};
    static constexpr unsigned kDirectFirst=2,kDirectCount=4,kPages=8192;
    DirectSlot direct[kDirectCount]{};
    int shownDirect=-1,pendingDirect=-1;
    uint64_t directClock=0;
    unsigned directFrames=0;

    bool dropDirect(unsigned index) {
        auto &slot=direct[index];
        if(!slot.memory)return true;
        if(!uke_ufs_dpu_map_external(kDirectFirst+index,nullptr,0))return false;
        slot.memory->complete();
        slot.memory->release();
        slot={};
        return true;
    }

    void releaseDirect(bool retired) {
        for(unsigned i=0;i<kDirectCount;++i) {
            if(!direct[i].memory)continue;
            if(!retired || !dropDirect(i))
                printf("[dp dpu] direct slot %u retained after incomplete retire\n",i);
        }
    }

    int mapSurface(IOMemoryDescriptor *memory,uint64_t required) {
        ++directClock;
        int pick=-1;
        for(unsigned i=0;i<kDirectCount;++i) {
            auto &slot=direct[i];
            if(slot.memory==memory && slot.iova && slot.mappedBytes>=required) {
                slot.used=directClock;
                return int(i);
            }
            if(int(i)==shownDirect || int(i)==pendingDirect)continue;
            if(pick<0 || slot.used<direct[pick].used)pick=int(i);
        }
        if(pick<0 || !dropDirect(unsigned(pick)))return -1;
        auto *pages=static_cast<uint64_t *>(IOMallocData(kPages*sizeof(uint64_t)));
        if(!pages)return -1;
        const unsigned count=native_dpu_wire_pages(memory,pages,kPages,required);
        uint32_t iova=0;
        if(count) {
            memory->retain();
            iova=uke_ufs_dpu_map_external(kDirectFirst+unsigned(pick),pages,count);
            // A failed TLB sync may leave this IOVA visible. Keep the pages
            // until the mapping can be removed or the controller retires.
            direct[pick]={memory,iova,directClock,uint64_t(count)*4096};
        }
        IOFreeData(pages,kPages*sizeof(uint64_t));
        return iova?pick:-1;
    }

    static IOReturn submitSurface(void *owner,IOMemoryDescriptor *memory,uint32_t offset,uint32_t pitch,
                                  const NativeExternalScanoutRect &target) {
        auto &self=*static_cast<DpDesktopScanout *>(owner);
        if(self.faulted || self.pendingDeadline || !self.dpu.healthy() || !&uke_ufs_dpu_map_external)
            return kIOReturnNotReady;
        if(!target.width || !target.height || target.x>self.width || target.y>self.height ||
           target.width>self.width-target.x || target.height>self.height-target.y)return kIOReturnBadArgument;
        const uint64_t bytes=memory->getLength();
        const uint64_t span=uint64_t(target.height-1)*pitch+target.width*4u;
        if(pitch<target.width*4u || pitch>0xffff || (pitch&63) || (offset&63) ||
           offset>bytes || span>bytes-offset)return kIOReturnBadArgument;
        return self.queueDirect(memory,offset,pitch,target,target.width,target.height,false);
    }

    static IOReturn submitXr10(void *owner,IOMemoryDescriptor *memory,uint32_t offset,uint32_t pitch,
                               uint32_t sourceWidth,uint32_t sourceHeight,const NativeExternalScanoutRect &target) {
        auto &self=*static_cast<DpDesktopScanout *>(owner);
        if(self.faulted || self.pendingDeadline || !self.dpu.healthy() || !&uke_ufs_dpu_map_external)
            return kIOReturnNotReady;
        if(!UkeDpDpu::xr10Geometry(sourceWidth,sourceHeight,target.width,target.height) ||
           target.x>self.width || target.y>self.height ||
           target.width>self.width-target.x || target.height>self.height-target.y ||
           pitch<sourceWidth*4u || pitch>0xffff || (pitch&63) || (offset&63))
            return kIOReturnBadArgument;
        return self.queueDirect(memory,offset,pitch,target,sourceWidth,sourceHeight,true);
    }

    IOReturn queueDirect(IOMemoryDescriptor *memory,uint32_t offset,uint32_t pitch,
                         const NativeExternalScanoutRect &target,unsigned sw,unsigned sh,bool xr10) {
        // A 4K b3a8 descriptor includes alpha past the existing 32 MiB
        // slot limit. VIG reads the bounded, opaque color plane only.
        const uint64_t required=offset+uint64_t(sh-1)*pitch+sw*4u;
        const int slot=mapSurface(memory,required);
        if(slot<0)return kIOReturnNoResources;
        const uint32_t address=direct[slot].iova+offset;
        if(!dpu.queueFlip(address,pendingFrame,pitch,&target,xr10,sw,sh))return kIOReturnBusy;
        pendingDirect=slot;
        clock_interval_to_deadline(80,kMillisecondScale,&pendingDeadline);
        if(native_legacy_dp_debug() && (++directFrames<=3 || directFrames%120==0))
            printf("[dp dpu] direct IOSurface frame=%u slot=%d pitch=%u offset=%u dst=%u,%u %ux%u xr10=%u src=%ux%u\n",
                   directFrames,slot,pitch,offset,target.x,target.y,target.width,target.height,unsigned(xr10),sw,sh);
        return kIOReturnSuccess;
    }

    static IOReturn acquire(void *owner, void **pixels, uint32_t *pitch, uint32_t *length) {
        auto &self=*static_cast<DpDesktopScanout *>(owner);
        if (self.faulted || self.pendingDeadline || !self.dpu.healthy()) return kIOReturnNotReady;
        const unsigned slot=self.shown^1;
        *pixels=self.buffers.pixels(slot);
        *pitch=self.buffers.stride();
        *length=self.buffers.bytes(slot);
        return kIOReturnSuccess;
    }
    static IOReturn submit(void *owner) {
        auto &self=*static_cast<DpDesktopScanout *>(owner);
        const unsigned slot=self.shown^1;
        const unsigned frame=native_legacy_dp_debug()?++self.submitted:0;
        if(frame && (frame<=6 || frame%120<=1)) {
            const auto *pixels=static_cast<const uint32_t *>(self.buffers.pixels(slot));
            const unsigned stride=self.buffers.stride()/4;
            unsigned lit=0;
            uint32_t hash=2166136261u;
            // Sample only, without logging framebuffer contents.
            for(unsigned y=0;y<9;++y) {
                const auto *row=pixels+((2*y+1)*self.height/18)*stride;
                for(unsigned x=0;x<16;++x) {
                    const uint32_t rgb=row[(2*x+1)*self.width/32]&0xffffffu;
                    lit+=rgb!=0;
                    hash=(hash^rgb)*16777619u;
                }
            }
            printf("[dp dpu] composed frame=%u slot=%u nonblack=%u/144 checksum=%08x\n",frame,slot,lit,hash);
        }
        self.buffers.clean(slot,self.height);
        if (!self.dpu.queueFlip(self.buffers.address(slot),self.pendingFrame,self.buffers.stride())) return kIOReturnBusy;
        self.pendingDirect=-1;
        clock_interval_to_deadline(80,kMillisecondScale,&self.pendingDeadline);
        return kIOReturnSuccess;
    }
    static IOReturn poll(void *owner) {
        auto &self=*static_cast<DpDesktopScanout *>(owner);
        if (!self.pendingDeadline) return kIOReturnNotReady;
        if (self.dpu.flipLatched(self.pendingFrame) && self.dpu.healthy()) {
            if(self.pendingDirect<0)self.shown^=1;
            self.shownDirect=self.pendingDirect;
            self.pendingDirect=-1;
            self.pendingDeadline=0;
            return kIOReturnSuccess;
        }
        if (mach_absolute_time()<self.pendingDeadline) return kIOReturnBusy;
        // A late latch can still consume either slot. Keep both until stop().
        self.faulted=true;
        return kIOReturnTimeout;
    }
};

// CTL0 owns the built-in framebuffer until a trained DP session needs VIG0.
// Release it only after CTL2 no longer scans the shared pipe.
struct PanelDmaLease {
    bool acquired=false;

    bool acquire(UkeDpWait wait,void *context) {
        uint64_t deadline=0;
        clock_interval_to_deadline(3000,kMillisecondScale,&deadline);
        do {
            const auto result=native_legacy_panel_route_dma(true);
            if(result==kIOReturnSuccess){acquired=true;return true;}
            // A live 120 Hz panel can retire its pending flip between short
            // retries. The HPD wait can otherwise sample the same busy phase.
            if(result==kIOReturnBusy) {
                IOSleep(1);
            } else {
                if(result!=kIOReturnNotReady || wait(context)==UkeDpActivity::Disconnect)break;
            }
        } while(mach_absolute_time()<deadline);
        printf("[dp xr10] panel DMA1 handoff unavailable\n");
        return false;
    }

    ~PanelDmaLease() {
        if(!acquired)return;
        for(unsigned attempt=0;attempt<80;++attempt) {
            const auto result=native_legacy_panel_route_dma(false);
            if(result==kIOReturnSuccess)return;
            if(result!=kIOReturnBusy)break;
            IOSleep(1);
        }
        printf("[dp xr10] panel VIG0 restore unavailable\n");
    }
};

bool showDpVideo(DpPixelClock &clock, const Registers &link, DpAux &aux,
                 const UkeDpVideo::Mode &mode, const DpDisplayInfo &display,
                 uint8_t rate, UkeDpWait wait, void *context) {
    UkeDpVideo::TransferUnit tu{};
    if (!wait || !UkeDpVideo::transferUnit(mode, rate, tu)) return false;
    const bool debug=native_legacy_dp_debug();
    const bool desktop=display.desktop,scanout=display.modes.wideBus;
    unsigned directXr10=0;
    PE_parse_boot_argn("uke-dp-xr10",&directXr10,sizeof(directXr10));
    const bool xr10=desktop && directXr10==1;
    if(scanout && !UkeDpVideo::wideBusCompatible(mode))return false;
    const unsigned pixelClock=UkeDpVideo::streamPixelClock(mode,scanout);
    Registers p0;
    if (!p0.map(0x0af56000, 0x9c) || !clock.start(pixelClock, rate)) {
        printf("[dp video] DPTX0 pixel clock or P0 mapping failed pixel=%u\n", pixelClock);
        return false;
    }
    constexpr unsigned bist = 0x00, timingEnable = 0x10, stateCtrl = 0x04, mainLinkReady = 0x40;
    constexpr unsigned asyncFifo = 0x88, dpuInput = 1u<<8;
    const uint32_t oldEnable=p0.read(timingEnable);
    if ((p0.read(bist)&1) || (oldEnable&(1|dpuInput))) {
        printf("[dp video] DPTX0 P0 generator already owned\n");
        return false;
    }

    unsigned reserve = 0;
    const bool reserveOnly = PE_parse_boot_argn("uke-dp-ext-buffers", &reserve, sizeof reserve) && reserve == 1;
    DpExternalBuffers external;
    if ((scanout || reserveOnly) && !external.prepare(mode)) {
        printf("[dp ext] scanout buffer preparation failed\n");
        if (scanout) return false;
    }
    PanelDmaLease panel;
    DpuDpScanout dpu;
    if (scanout) {
        external.paint(mode, desktop);
        if(xr10 && !panel.acquire(wait,context))return false;
        if (!dpu.start(mode,external,xr10))return false;
    }

    // MSA and transfer units describe physical pixels. The OEM wide-bus
    // path halves the clock in the RCG and doubles Mvid to compensate.
    const unsigned htotal = mode.htotal(), vtotal = mode.vtotal();
    const unsigned hstart = mode.hsync + mode.hback, vstart = mode.vsync + mode.vback;
    const auto ratio=UkeDpVideo::streamRatio(clock.m,clock.n,scanout,rate);
    struct Write {unsigned reg; uint32_t value;};
    const Write linkConfig[] = {
        {0x10, ratio.mvid}, {0x18, ratio.nvid},
        {0x1c, vtotal << 16 | htotal}, {0x20, vstart << 16 | hstart},
        {0x24, mode.vsync << 16 | mode.hsync | (mode.hlow ? 1u << 15 : 0) | (mode.vlow ? 1u << 31 : 0)},
        {0x28, mode.height << 16 | mode.width},
        {0x2c, 0x21}, // 8 bpc RGB, synchronous clock.
        {0x30, tu.delay << 16 | tu.valid}, {0x34, tu.boundary2()}, {0x4c, tu.size - 1},
        {0x180, 0}, // DP_COMPRESSION_MODE_CTRL: native RGB8 SST is uncompressed.
    };
    const unsigned displayStart = vstart * htotal + hstart;
    const unsigned displayEnd = (vtotal - mode.vfront) * htotal - mode.hfront - 1;
    const Write p0Config[] = {
        {0x14, p0.read(0x14) & ~(1u << 4)}, // normal-width pixel bus
        {0x18, htotal << 16 | mode.hsync},
        {0x1c, vtotal * htotal}, {0x20, 0},
        {0x24, mode.vsync * htotal}, {0x28, 0},
        {0x2c, displayStart}, {0x30, 0}, {0x34, displayEnd}, {0x38, 0},
        {0x3c, 0}, {0x40, 0}, {0x44, 0}, {0x48, 0},
        {0x4c, (htotal - mode.hfront - 1) << 16 | hstart}, {0x50, 0}, {0x58, 0},
        {0x60, 0x100}, {0x64, 5}, // msm_dp_panel_tpg_enable: checkered pattern, RGB 8-bit
    };
    uint32_t oldLink[sizeof linkConfig / sizeof linkConfig[0]], oldP0[sizeof p0Config / sizeof p0Config[0]];
    auto apply = [](const Registers &regs, const auto &rows, auto &saved) {
        for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            saved[i] = regs.read(rows[i].reg);
            regs.write(rows[i].reg, rows[i].value);
        }
    };
    auto restore = [](const Registers &regs, const auto &rows, const auto &saved) {
        for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; ++i) regs.write(rows[i].reg, saved[i]);
    };
    const uint32_t oldMisr=debug && scanout?link.read(0xd0):0;
    link.write(stateCtrl, 0);
    apply(link, linkConfig, oldLink);
    if (debug && scanout) link.write(0xd0, 0x501); // OEM DPTX controller MISR, one frame.
    if (native_legacy_dp_debug()) printf("[dp video] stream %ux%u clock=%u rate=%02x rcg=%u/%u msa=%u/%u tu=%u/%u delay=%u boundary2=%08x\n",
           mode.width, mode.height, mode.pixel, rate, clock.m, clock.n, ratio.mvid, ratio.nvid,
           tu.valid, tu.size, tu.delay, tu.boundary2());
    constexpr unsigned dscDto=0x7c,dscCount=0x84;
    uint32_t oldFifo=0,oldDto=0,oldDtoCount=0;
    if (scanout) {
        oldP0[0] = p0.read(p0Config[0].reg);
        p0.write(p0Config[0].reg, p0Config[0].value | (1u<<4));
        // OEM dp_catalog_ctrl_config_ctrl selects the DPU pixel stream
        // (bit 8). Bit 0 selects the transmitter's internal generator.
        oldFifo=p0.read(asyncFifo);
        p0.write(asyncFifo,(link.read(0)>>8)&1);
        oldDto=p0.read(dscDto);
        oldDtoCount=p0.read(dscCount);
        p0.write(dscDto,0);
        p0.write(dscCount,0);
        p0.write(timingEnable,oldEnable|dpuInput);
        dpu.enable();
        if (native_legacy_dp_debug()) printf("[dp dpu] P0 timing=%08x async=%08x\n",p0.read(timingEnable),p0.read(asyncFifo));
        if (debug) {
            const auto intfTiming=UkeDpDpu::timing(mode,true);
            printf("[dp dpu] widebus pixel_khz=%u horizontal=%u mvid=%u nvid=%u p0=%08x\n",
                   pixelClock,intfTiming.hsync>>16,ratio.mvid,ratio.nvid,p0.read(p0Config[0].reg));
        }
    } else {
        apply(p0, p0Config, oldP0);
        p0.update(bist, 1, 1);
        p0.update(timingEnable, 1, 1);
    }
    const uint32_t firstFrame = scanout ? dpu.frame() : 0;
    link.write(stateCtrl, 0x80); // SEND_VIDEO

    bool ready = false;
    for (unsigned elapsed = 0; elapsed < 500; ++elapsed) {
        const bool frameReady = !scanout || (dpu.healthy() && dpu.initialCommitRetired() && dpu.frame() != firstFrame);
        if ((link.read(mainLinkReady) & 1) && frameReady) {ready = true; break;}
        IOSleep(1);
    }
    if (scanout && (!ready || debug))
        printf("[dp dpu] video %ux%u ready=%u frame=%u->%u link=%08x fsr=%08x\n",
               mode.width, mode.height, ready, firstFrame, dpu.frame(), link.read(mainLinkReady),
               native_ufs_dpu_live(1));
    else if (!scanout && (!ready || debug))
        printf("[dp video] TPG %ux%u clock=%u rate=%02x tu=%u/%u delay=%u ready=%u state=%08x\n",
               mode.width, mode.height, mode.pixel, rate, tu.valid, tu.size, tu.delay, ready, link.read(mainLinkReady));
    if (debug && scanout) dpu.sample(link, aux, "startup");
    DpSinkCrc receiver(aux);
    if (debug && ready && scanout) {
        receiver.start();
        receiver.sample("startup");
        receiver.symbolErrors();
    }
    DpDesktopScanout desktopScanout{external,dpu,mode.width,mode.height};
    const NativeExternalDisplayLink connection{mode.width,mode.height,mode.pixel,
        mode.hfront,mode.hsync,mode.hback,mode.vfront,mode.vsync,mode.vback,mode.hlow,mode.vlow,
        display.edid[0],display.length,&desktopScanout,&DpDesktopScanout::acquire,&DpDesktopScanout::submit,
        &DpDesktopScanout::poll,&DpDesktopScanout::submitSurface,xr10?&DpDesktopScanout::submitXr10:nullptr};
    IOReturn desktopStatus=kIOReturnNotFound;
    uint32_t lastFlip = firstFrame;
    uint32_t lastSample=firstFrame;
    unsigned sampleCount=0;
    unsigned slot = 0;
    while (ready) {
        // A dock present during boot may train before the graphics family
        // starts. Retry that ordering only, once the original service exists.
        if(desktop && desktopStatus==kIOReturnNotFound) {
            desktopStatus=uke_dp_set_external_display(&connection);
            if(desktopStatus!=kIOReturnNotFound && (desktopStatus!=kIOReturnSuccess || debug))
                printf("[dp desktop] connect status=%x\n",desktopStatus);
        }
        const auto activity = wait(context);
        if (activity == UkeDpActivity::Disconnect) break;
        if (activity == UkeDpActivity::Attention) {
            UkeDpLink::Status status;
            if (aux.read(0x202, status.bytes, sizeof status.bytes) == sizeof status.bytes && !status.equalized()) {
                printf("[dp video] HPD IRQ lost link lanes=%02x align=%02x\n", status.bytes[0], status.bytes[2]);
                break;
            }
        }
        if (scanout && !dpu.healthy()) {
            printf("[dp dpu] scanout failed fsr=%08x\n", native_ufs_dpu_live(1));
            break;
        }
        if (debug && scanout && dpu.frame()-lastSample>=120) {
            dpu.sample(link,aux,"running");
            if (sampleCount++ % 3 == 0) {
                receiver.sample("running");
                receiver.symbolErrors();
            }
            lastSample=dpu.frame();
        }
        if (scanout && !desktop && dpu.frame() - lastFlip >= 45) {
            slot ^= 1;
            if (!dpu.flip(external.address(slot))) {
                printf("[dp dpu] swap did not latch slot=%u frame=%u\n", slot, dpu.frame());
                break;
            }
            lastFlip = dpu.frame();
            if (native_legacy_dp_debug()) printf("[dp dpu] swap slot=%u iova=%08x frame=%u\n", slot, external.address(slot), lastFlip);
        }
    }
    if (desktopStatus==kIOReturnSuccess) uke_dp_set_external_display(nullptr);
    if (debug && scanout) {
        receiver.stop();
        dpu.sample(link,aux,"final");
    }
    const bool idle = ready && aux.waitVideoIdle(link);
    link.write(stateCtrl, 0);
    if (scanout) {
        const bool retired = dpu.stop();
        if(desktop)desktopScanout.releaseDirect(retired);
        if (!idle || !retired || debug) printf("[dp dpu] stopped idle=%u retired=%u\n", idle, retired);
        if (!retired) {
            external.retainWhileBusy();
            panel.acquired=false; // VIG0 may still be owned by CTL2.
        }
        p0.write(timingEnable,oldEnable);
        p0.write(asyncFifo,oldFifo);
        p0.write(dscDto,oldDto);
        p0.write(dscCount,oldDtoCount);
        p0.write(p0Config[0].reg, oldP0[0]);
    } else {
        p0.update(timingEnable, 1, 0);
        p0.update(bist, 1, 0);
        restore(p0, p0Config, oldP0);
        if (!idle || debug) printf("[dp video] TPG stopped idle=%u\n", idle);
    }
    if (debug && scanout) link.write(0xd0, oldMisr & ~((1u << 9) | (1u << 10)));
    restore(link, linkConfig, oldLink);
    return ready;
}

bool trainDpPattern(const Registers &link, const Registers &phy, DpAux &aux, uint8_t rate, uint8_t pattern,
                    uint8_t auxInterval, UkeDpLink::SignalLevels &levels) {
    UkeDpLink::Status status;
    const bool equalization = pattern != 1;
    constexpr unsigned stateCtrl = 4, mainLinkReady = 0x40;
    const uint32_t state = 1u << (pattern - 1);
    const uint8_t sinkPattern = 0x20 | pattern; // Scrambling off during TPS1–3.
    link.write(stateCtrl, 0);
    link.write(stateCtrl, state);
    const bool patternReady = link.wait(mainLinkReady, state << 3, state << 3, 1000);
    if (!patternReady || !aux.write(0x102, &sinkPattern, 1)) {
        printf("[dp train] TPS%u start failed source_ready=%u ready=%08x aux=%08x\n",
               pattern, patternReady, link.read(mainLinkReady), aux.status);
        return false;
    }
    unsigned repeatedSwing = 0;
    for (unsigned attempt = 0; attempt < (equalization ? 6u : 12u); ++attempt) {
        setDpLevels(phy, rate, levels);
        const uint8_t laneSet[] = {levels.laneSet(), levels.laneSet()};
        if (!aux.write(0x103, laneSet, sizeof laneSet)) break;
        const unsigned delay = UkeDpLink::trainingDelayUs(auxInterval, equalization);
        if (delay >= 1000) IOSleep((delay + 999) / 1000);
        else IODelay(delay);
        if (aux.read(0x202, status.bytes, sizeof status.bytes) != sizeof status.bytes) break;
        const bool success = equalization ? status.equalized() : status.clockRecovered();
        if (native_legacy_dp_debug()) printf("[dp train] TPS%u attempt=%u lanes=%02x align=%02x request=%02x swing=%u pre=%u\n",
               pattern, attempt, status.bytes[0], status.bytes[2], status.bytes[4], levels.swing, levels.pre);
        if (success) return true;
        if ((equalization && !status.clockRecovered()) || (!equalization && levels.swing == 3)) break;
        const auto requested = UkeDpLink::SignalLevels::requested(status);
        repeatedSwing = requested.swing == levels.swing ? repeatedSwing + 1 : 0;
        if (!equalization && repeatedSwing >= 4) break;
        levels = requested;
    }
    printf("[dp train] TPS%u failed aux=%08x\n", pattern, aux.status);
    return false;
}

bool attemptDpTrain(const Registers &disp, const Registers &phy, DpAux &aux, int lane,
                    const uint8_t (&caps)[16], uint8_t rate, const UkeDpVideo::Mode *mode,
                    const DpDisplayInfo *display, UkeDpWait wait, void *context) {
    if (!startDpPhy(phy, lane != 0, rate)) return false;
    DpLinkClocks clocks(disp);
    Registers link;
    if (!clocks.start() || !link.map(0x0af55000, 0x770)) {
        printf("[dp train] DPTX0 link clock or mapping failed rate=%02x\n", rate);
        return false;
    }
    constexpr unsigned mainCtrl = 0, stateCtrl = 4, configCtrl = 8, laneMap = 0x38, mainLinkLevels = 0x44;
    const uint32_t oldMain = link.read(mainCtrl);
    if (oldMain & 3) {
        printf("[dp train] main link already owned ctrl=%08x\n", oldMain);
        return false;
    }
    const uint32_t oldState = link.read(stateCtrl), oldConfig = link.read(configCtrl), oldMapping = link.read(laneMap);
    const uint32_t oldLevels=link.read(mainLinkLevels);
    // OEM MST and FEC configuration. The active route is synchronous SST.
    constexpr uint32_t mstMode=0x04000100u, fecEnable=1u<<12;
    const uint32_t sst=(oldMain & ~(mstMode|fecEnable)) | 0x02000000u;
    const bool enhanced = (caps[2] & 0x80) != 0;
    link.write(laneMap, 0xe4); // DPTX0 identity mapping; QMP handles cable orientation.
    link.write(configCtrl, (2u << 13) | (1u << 8) | (enhanced ? 0x40u : 0) | 0x17u);
    link.write(mainLinkLevels,(oldLevels&0xfe0u)|8u); // OEM safe-to-exit level for two lanes.
    link.write(mainCtrl,sst);
    link.write(mainCtrl,sst|2);
    link.write(mainCtrl,sst);
    link.write(mainCtrl,sst|1);
    if (native_legacy_dp_debug()) printf("[dp train] native SST ctrl=%08x levels=%08x\n",link.read(mainCtrl),link.read(mainLinkLevels));

    const uint8_t params[] = {rate, static_cast<uint8_t>(2u | (enhanced ? 0x80u : 0))};
    const uint8_t coding[] = {0, 1}; // SSC disabled, ANSI 8b/10b.
    const uint8_t eqPattern = (caps[2] & 0x40) ? 3 : 2;
    UkeDpLink::SignalLevels levels;
    const bool configured = aux.write(0x100, params, sizeof params) && aux.write(0x107, coding, sizeof coding);
    const bool recovered = configured && trainDpPattern(link, phy, aux, rate, 1, caps[0xe], levels);
    const bool equalized = recovered && trainDpPattern(link, phy, aux, rate, eqPattern, caps[0xe], levels);
    const uint8_t off = 0;
    const bool cleared = !configured || aux.write(0x102, &off, 1);
    if (!equalized || !cleared || native_legacy_dp_debug()) printf("[dp train] result lane=%d rate=%02x configured=%u cr=%u eq=%u cleared=%u\n",
           lane, rate, configured, recovered, equalized, cleared);
    const bool trained = equalized && cleared;
    DpPixelClock pixelClock(disp);
    const bool presented = trained && (!mode || showDpVideo(pixelClock, link, aux, *mode, *display, rate, wait, context));
    link.write(stateCtrl, oldState);
    link.write(mainCtrl, oldMain);
    if (mode) {
        constexpr unsigned mainLinkReady = 0x40;
        const uint32_t before = link.read(mainLinkReady);
        const bool reset = aux.resetTransmitter();
        if (!reset || native_legacy_dp_debug()) printf("[dp video] DPTX0 reset=%u ready=%08x->%08x\n", reset, before, link.read(mainLinkReady));
    }
    link.write(configCtrl, oldConfig);
    link.write(laneMap, oldMapping);
    link.write(mainLinkLevels,oldLevels);
    return presented;
}

void trainDpLink(const Registers &disp, const Registers &phy, DpAux &aux, int lane, const uint8_t (&caps)[16],
                 const DpDisplayInfo *display, UkeDpWait wait, void *context) {
    if (caps[0] < 0x10 || caps[0] > 0x14 || (caps[2] & 0x1f) < 2 || caps[1] < UkeDpLink::kRbr) return;
    uint8_t power = 0;
    if (caps[0] >= 0x11 && aux.read(0x600, &power, 1) == 1 && (power & 7) != 1) {
        power = (power & ~7u) | 1; // DP_SET_POWER_D0
        if (!aux.write(0x600, &power, 1)) {
            printf("[dp train] receiver D0 failed aux=%08x\n", aux.status);
            return;
        }
        IOSleep(1);
    }
    if (display && display->desktop) {
        const auto &mode=display->preferred;
        const auto rate=UkeDpVideo::nativeRate(mode,caps[1]);
        if (native_legacy_dp_debug()) printf("[dp video] native EDID %ux%u clock=%u rate=%02x max=%02x\n",
               mode.width,mode.height,mode.pixel,rate,caps[1]);
        if (!rate) return;
        (void)attemptDpTrain(disp,phy,aux,lane,caps,rate,&mode,display,wait,context);
        phy.write(kDpPhyPower,2);
        IOSleep(1);
        initDpAuxPhy(phy);
        return;
    }
    constexpr uint8_t rates[] = {UkeDpLink::kHbr2, UkeDpLink::kHbr, UkeDpLink::kRbr};
    for (uint8_t rate : rates) {
        if (rate > caps[1]) continue;
        const UkeDpVideo::Mode *mode = nullptr;
        if (display) {
            mode = &display->modes.atRate(rate);
            if (!mode->pixel) continue;
        }
        if (native_legacy_dp_debug()) printf("[dp train] trying lane=%d rate=%02x max=%02x\n", lane, rate, caps[1]);
        const bool trained = attemptDpTrain(disp, phy, aux, lane, caps, rate, mode, display, wait, context);
        phy.write(kDpPhyPower, 2); // qmp_combo_dp_power_off; USB COM stays in combined mode.
        IOSleep(1);
        initDpAuxPhy(phy);
        if (trained) break;
    }
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

void uke_usb2_log_phy() {
    Registers phy;
    if (!phy.map(0x88e3000, 0x1000)) return;
    printf("[usb flight] phy utmi=%08x por=%08x common=%08x bias=%08x div=%08x/%08x ctrl=%08x cfg0=%08x fsel=%08x apb=%08x\n",
           phy.read(0x3c), phy.read(0x50), phy.read(0x54), phy.read(0x58),
           phy.read(0x5c), phy.read(0x60), phy.read(0x64), phy.read(0x94), phy.read(0xb8), phy.read(0x130));
}

int uke_usb2_hardware_init() {
    if (!gGcc.map(0x100000, 0x53000)) return -1;
    native_usb_stats[0] = 2;
    if (!voteRails(false)) return -2;
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
    if (!repeater(false) || !routeWcd()) {
        printf("[usb] uke repeater=%08x wcd=%08x\n", native_usb_stats[8], native_usb_stats[32]);
        return -6;
    }
    native_usb_stats[0] = 7;
    if (!initPhy()) return -7;
    IOSleep(2);
    return 0;
}

bool uke_usb3_hardware_init() {
    return voteRails(true) && initSuperSpeedPhy(-1, false);
}

bool uke_usb3_set_orientation(int lane, bool displayPort) {
    return lane >= (displayPort ? 0 : -1) && lane <= 1 && initSuperSpeedPhy(lane, displayPort);
}

bool uke_usb_dp_route_aux(int lane) {
    if (lane < -1 || lane > 1) return false;
    IOReturn result = kIOReturnNotReady;
    for (unsigned attempt = 0; attempt < 6; ++attempt) {
        result = uke_backlight_with_wcd_bus(programWcdAux, &lane);
        if (result != kIOReturnBusy && result != kIOReturnNotReady) break;
        IOSleep(5);
    }
    return result == kIOReturnSuccess;
}

void uke_usb_dp_read_dpcd(int lane, UkeDpWait wait, void *context) {
    // The display clocks and MMIO match the OEM Cliffs dispcc and DP resources.
    // The existing panel owns MDSS core power and the shared AHB clock.
    constexpr unsigned mdssGdsc = 0x9000, mdssGdscStatus = 0x9004, ahbClock = 0x80a4;
    constexpr unsigned auxClock = 0x8058, auxRcg = 0x81bc, auxRcgConfig = 0x81c0;
    constexpr uint32_t update = 1u, halted = 1u << 31, cfgPowerOn = 1u << 16, rcgMask = 0x371f;
    Registers disp, phy, dptx;
    if (!disp.map(0x0af00000, 0x10000) || !phy.map(0x088e8000, 0x3000) ||
        !dptx.map(0x0af54000, 0x1000)) {
        printf("[dp aux] register mapping failed\n");
        return;
    }
    if (!(disp.read(mdssGdsc) & halted) && !(disp.read(mdssGdscStatus) & cfgPowerOn)) {
        printf("[dp aux] MDSS domain off\n");
        return;
    }
    if ((disp.read(ahbClock) & halted) || (phy.read(0) & 3u) != 3 ||
        (phy.read(0x10) & 3u) != (2u | static_cast<unsigned>(lane))) {
        printf("[dp aux] clock or combined route unavailable lane=%d\n", lane);
        return;
    }

    const uint32_t oldClock = disp.read(auxClock), oldConfig = disp.read(auxRcgConfig);
    const bool clockOwned = !(oldClock & 1u);
    auto releaseClock = [&] {
        if (!clockOwned) return;
        disp.update(auxClock, 1, 0);
        disp.update(auxRcgConfig, rcgMask, oldConfig & rcgMask);
        disp.update(auxRcg, 0, update);
        if (!disp.wait(auxRcg, update, 0)) printf("[dp aux] AUX RCG restore timed out\n");
    };
    if (clockOwned) {
        disp.update(auxRcgConfig, rcgMask, 1); // BI_TCXO 19.2 MHz, divide by one.
        disp.update(auxRcg, 0, update);
        if (!disp.wait(auxRcg, update, 0)) {
            printf("[dp aux] AUX RCG update timed out\n");
            releaseClock();
            return;
        }
        disp.update(auxClock, 1, 1);
    } else if ((oldConfig & rcgMask) != 1) {
        printf("[dp aux] unexpected running AUX clock cfg=%08x\n", oldConfig);
        return;
    }
    if (!disp.wait(auxClock, halted | 1u, 1)) {
        printf("[dp aux] AUX clock failed branch=%08x\n", disp.read(auxClock));
        releaseClock();
        return;
    }

    constexpr unsigned auxCtrl = 0x230, timeout = 0x23c, limits = 0x240;
    const uint32_t oldCtrl = dptx.read(auxCtrl);
    if (oldCtrl & 3u) {
        printf("[dp aux] AUX controller busy ctrl=%08x\n", oldCtrl);
        releaseClock();
        return;
    }
    initDpAuxPhy(phy);
    const uint32_t oldTimeout = dptx.read(timeout), oldLimits = dptx.read(limits);
    dptx.write(timeout, 0xffff);
    dptx.write(limits, 0xffff);
    dptx.write(auxCtrl, oldCtrl | 1);
    DpAux aux(dptx);
    uint8_t caps[16]{};
    unsigned bytes = aux.read(0, caps, sizeof caps);
    for (unsigned attempt=1; !bytes && aux.timedOut() && wait && attempt<=2; ++attempt) {
        if (wait(context)==UkeDpActivity::Disconnect) break;
        printf("[dp aux] retry DPCD lane=%d attempt=%u after TIMEOUT\n",lane,attempt);
        bytes=aux.read(0,caps,sizeof caps);
    }
    if (bytes!=sizeof caps || native_legacy_dp_debug()) printf("[dp aux] DPCD lane=%d status=%08x phy_irq=%08x bytes=%u rev=%02x rate=%02x lanes=%u\n",
           lane, aux.status, aux.phyStatus(), bytes, caps[0], caps[1], caps[2] & 0x1f);
    unsigned enableTrain = 0;
    if (bytes == sizeof caps && PE_parse_boot_argn("uke-dp-train", &enableTrain, sizeof enableTrain) && enableTrain == 1) {
        unsigned enableVideo = 0, desktopOption=0, scanoutOption=0;
        DpDisplayInfo display;
        display.desktop=PE_parse_boot_argn("uke-dp-desktop",&desktopOption,sizeof desktopOption) && desktopOption==1;
        display.modes.wideBus=display.desktop ||
            (PE_parse_boot_argn("uke-dp-dpu",&scanoutOption,sizeof scanoutOption) && scanoutOption==1);
        const bool video = PE_parse_boot_argn("uke-dp-video", &enableVideo, sizeof enableVideo) && enableVideo == 1;
        readDpEdid(aux, video ? &display : nullptr);
        if (!video || display.desktop || display.modes.hbr2.pixel || display.modes.hbr.pixel || display.modes.rbr.pixel)
            trainDpLink(disp, phy, aux, lane, caps, video ? &display : nullptr, wait, context);
        else
            printf("[dp video] EDID has no timing in the two-lane RGB8 profile\n");
    }
    aux.clear();
    dptx.write(auxCtrl, oldCtrl);
    dptx.write(timeout, oldTimeout);
    dptx.write(limits, oldLimits);
    releaseClock();
}

bool uke_usb_set_repeater_host(bool host, bool restartPhy) {
    const bool ok = repeater(host);
    printf("[usb otg] repeater host=%u ready=%u status=%08x\n", host, ok, native_usb_stats[8]);
    if (!ok || !restartPhy) return ok;
    // msm_eusb2_phy_init sets up the repeater before resetting the PHY.
    const bool ready = initPhy();
    printf("[usb otg] eUSB2 role=%s ready=%u\n", host ? "host" : "device", ready);
    return ready;
}

bool uke_usb_otg_enabled() {
    unsigned usb = 0, otg = 0;
    return PE_parse_boot_argn("usb", &usb, sizeof usb) && usb == 1 &&
           PE_parse_boot_argn("uke-usb-otg", &otg, sizeof otg) && otg == 1;
}

bool uke_usb3_read_orientation(UkeUsbOrientation &state) {
    // Called by the USB flight recorder after QMP has powered up. The OEM
    // QMP COM TYPEC_STATUS register (0x24) exposes PORTSELECT_RAW in bit 0.
    static Registers gpio, phy;
    if ((!gpio.base && !gpio.map(0x0f17a000, 0x1000)) ||
        (!phy.base && !phy.map(0x088e8000, 0x3000))) return false;
    state = {gpio.read(0), gpio.read(4), phy.read(0x10), phy.read(0x24), phy.read(0x1c14)};
    return true;
}
