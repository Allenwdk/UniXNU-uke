// SPDX-License-Identifier: BSD-2-Clause-Patent
// Panel brightness for uke, adapted from sheng a68ee4ee (boot argument sheng-backlight=1, started by BacklightProbe.cpp). The panel's backlight is two Kinetic KTZ8866 drivers at I2C
// address 0x11, one on QUP wrap0 SE0 (i2c@a80000) and one on QUP wrap1 SE4 (i2c@890000);
// both get the same 11-bit level in BL_BRT_LSB (0x04, bits 2:0) and BL_BRT_MSB (0x05), as the mainline ktz8866 driver writes it.
// Clear BL_EN bit 6 for off without dimming or rewriting the saved brightness.
// Restore/verify a changed brightness before enabling, preserving adopted bias/sink configuration.
// Keep the firmware's I2C protocol image, restore the Uke clock, I/O and FIFO configuration,
// then issue transfers through the event-driven core in GeniI2c.c.
// Transfers never wait: each engine's interrupt (SPI 353 for a, SPI 586 for b, level) only reads and clears the engine's status, records
// it and signals the service's work loop, which advances the transfer. Until interrupts are attached, or with uke-backlight-irq=0,
// the work loop polls the status every millisecond while a transfer runs. A step
// that gets no answer within 25 ms is cancelled, then aborted. A newer level replaces one not yet written, so a fade never queues up.
// The class cannot be called AppleARMBacklight: AppleARMPlatform already defines that class, and a duplicate name panics the kernel when
// this file's classes are registered (int-229); the service answers user space's conformance query for it instead (publishFirst). Whether
// CoreBrightness drives ShengBacklight is what native_backlight_stats[1] (brightness sets) shows. CoreBrightness (backboardd) looks for a backlight service with backlight-control = true, reads min and max from
// IODisplayParameters brightness and sets {brightness, FadePeriod (16.16 seconds)} through setProperties. A fade is stepped every 16 ms by a
// timer on the service's own work loop. iPadOS 27 reads a nits range and sets brightness-nits (signed 16.16) instead, and ramps by itself.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/thread_call.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include "GeniI2c.h"
#include "UkeBacklightState.h"

// [0] stage (1 mapped, 2 engines ready, 3 published, 4 published as AppleARMBacklight, negative: the step that failed), [1] brightness requests, [2] last effective output (0 when gated),
// [3] level read back from engine a at start, [4] from engine b, [5] transfer errors, [6] last M_IRQ status of a failed transfer,
// [7] engine a protocol and clock configuration (proto << 24 | M_CLK_CFG), [8] the same for engine b, [9] last error (engine << 8 | the
// GeniI2c.h error, or a setup code), [10] interrupts from engine a, [11] from engine b, [12] interrupt mode (0 polling, 1 attached, else
// the attach result), [13] transfer timeouts, [14] attach tries, [15] transfers done
extern "C" {extern volatile int32_t native_backlight_stats[16];}
// The last brightness asked for: [0] level, [1] fade in milliseconds; [2] request timer runs, [3] fades begun from a request; [4] interrupts
// with no status bit set, [5] the last status an interrupt saw.
extern "C" {volatile int32_t native_backlight_request[6];}
// Per origin: calls, status a/b, captured a/b, direct a/b, expired steps.
// Origin 0 initial, 1 IRQ callout on the work-loop gate, 2 deadline timer, 3 attach.
extern "C" {volatile uint32_t native_backlight_flow[4][8];}
// Per engine: nonempty primary IRQs, signal calls, signal returns, no inbox.
extern "C" {uint32_t native_backlight_signal[2][4];}
extern "C" {volatile uint32_t native_backlight_irq_wait_us[2][2];}
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));

namespace {
constexpr uint64_t kGcc = 0x100000, kSeA = 0xa80000, kSeB = 0x890000;
constexpr uint32_t kSpi[2] = {353, 586};
// gcc-cliffs.c: wrap0 core2x/core/mAHB/sAHB/s0; wrap1 core2x/core/mAHB/sAHB/s4.
constexpr uint32_t kGccVote[2] = {0x52008, 0x52010};
constexpr uint32_t kVoteBits[2] = {(1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22),
                                 (1u << 3) | (1u << 0) | (1u << 2) | (1u << 1) | (1u << 8)};
constexpr uint32_t kHalt[] = {0x23018, 0x23008, 0x23000, 0x23004, 0x18004, 0x23168, 0x23158, 0x23150, 0x23154, 0x1e4e4};
constexpr uint32_t kRcgCommand[] = {0x18010, 0x1e4f0};
constexpr uint32_t kRcgUpdate = 1, kRcgConfig = 4, kRcgRootOff = 1u << 31;
constexpr uint32_t kRcgRateMask = 0x371f, kRcgTcxoDiv1 = 1;
constexpr uint32_t kRcgDfsCommand = 0x14, kRcgDfsPerfConfig = 0x1c, kRcgDfsLevels = 8;
constexpr uint32_t kSeForceDefault = 0x20, kSeOutput = 0x24, kSeCgc = 0x28;
constexpr uint32_t kSeStatus = 0x40, kSeClkSelect = 0x7c, kSeIos = 0x908;
constexpr uint32_t kSeGsiEvent = 0xe18, kSeDmaGeneral = 0xe30;
constexpr uint32_t kSeActive = (1u << 0) | (1u << 12);
// Xiaomi i2c-msm-geni.c defaults to 400 kHz, also measured on both buses in stock recovery.
// Preserve the matching 19.2 MHz firmware handoff timing when present.
constexpr uint32_t kMclk = (2u << 4) | 1;
constexpr uint32_t kSclFirmware = 0x503018, kSclStock = (7u << 20) | (10u << 10) | 24;
constexpr uint8_t kAddress = 0x11, kBrtLsb = 0x04, kBrtMsb = 0x05, kBlEnable = 0x08, kBlEnableBit = 0x40;
// The range CoreBrightness works in. The top level stands for kMaxNits: the panel's luminance has not been measured, so these set the slider's
// scale, not an absolute brightness. kMidNits is CoreBrightness's own default.
constexpr uint32_t kMinNits = 2, kMidNits = 140, kMaxNits = 500;
constexpr uint32_t kMaxLevel = 2047, kStepMs = 16, kPollMs = 1, kStepDeadlineMs = 25, kAttachMs = 250, kAttachTries = 240;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
Window gGcc, gSe[2], gPins[4];
struct EngineIo {
    volatile uint32_t *va;
    IOSimpleLock *lock;
    uint32_t pending, mask;
    bool deferred;
    uint64_t firstPendingAt;
};
EngineIo gIo[2];
thread_call_t gIrqCall;
// The newest brightness asked for: goal << 16 | fade milliseconds, with bit 63 set while the work loop has not taken it.
volatile uint64_t gRequest;
bool gRuntimeReady;
// Per engine: written, read back, errors, retry count; no successful write is inferred from a failed transfer.
extern "C" {volatile int32_t native_backlight_engine[2][4];}
extern "C" {volatile uint32_t native_backlight_irq_watchdog[2];}

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
uint32_t rd(volatile uint32_t *base, uint32_t reg) {return base[reg / 4];}
void wr(volatile uint32_t *base, uint32_t reg, uint32_t value) {base[reg / 4] = value; __asm__ volatile("dsb sy" ::: "memory");}
uint32_t ioRead(void *ctx, uint32_t reg) {return rd(static_cast<EngineIo *>(ctx)->va, reg);}
void ioWrite(void *ctx, uint32_t reg, uint32_t value) {
    EngineIo &io = *static_cast<EngineIo *>(ctx);
    if (reg != GENI_M_IRQ_EN) {wr(io.va, reg, value); return;}
    const auto state = IOSimpleLockLockDisableInterrupt(io.lock);
    io.mask = value;
    wr(io.va, reg, io.deferred ? 0 : value);
    IOSimpleLockUnlockEnableInterrupt(io.lock, state);
}

uint32_t takeStatus(unsigned index, bool *watchdog, uint32_t *capturedBits=nullptr, uint64_t *capturedAt=nullptr) {
    EngineIo &io = gIo[index];
    const auto state = IOSimpleLockLockDisableInterrupt(io.lock);
    io.deferred = true;
    wr(io.va, GENI_M_IRQ_EN, 0);
    const uint32_t direct = rd(io.va, GENI_M_IRQ_STATUS);
    const uint32_t captured = io.pending;
    if(capturedBits)*capturedBits=captured;
    if(capturedAt)*capturedAt=io.firstPendingAt;
    io.pending = 0;
    io.firstPendingAt = 0;
    if (direct) wr(io.va, GENI_M_IRQ_CLEAR, direct);
    if (watchdog) *watchdog = direct && !captured;
    IOSimpleLockUnlockEnableInterrupt(io.lock, state);
    return captured | direct;
}

void rearmStatus(unsigned index) {
    EngineIo &io = gIo[index];
    const auto state = IOSimpleLockLockDisableInterrupt(io.lock);
    io.deferred = false;
    wr(io.va, GENI_M_IRQ_EN, io.mask);
    IOSimpleLockUnlockEnableInterrupt(io.lock, state);
}
void fail(int step, int error) {
    native_backlight_stats[9] = error; native_backlight_stats[0] = -step;
    printf("UKE BL: fail step=%d error=%d\n", step, error);
}
void bump(int index) {native_backlight_stats[index] = native_backlight_stats[index] + 1;}

bool configureEngine(unsigned i) {
    volatile uint32_t *const se = gSe[i].va;
    const uint32_t fw = rd(se, GENI_FW_REVISION), interface = rd(se, GENI_IF_DISABLE), status = rd(se, kSeStatus);
    const uint32_t inheritedMclk = rd(se, GENI_CLK_CFG), inheritedScl = rd(se, GENI_SCL_COUNTERS);
    const uint32_t cmd = kRcgCommand[i], cfg = cmd + kRcgConfig, dfs = rd(gGcc.va, cmd + kRcgDfsCommand);
    printf("UKE BL: handoff engine=%u fw=%x if=%x status=%x ios=%x rcg=%x/%x dfs=%x sel=%x mclk=%x scl=%x\n",
           i, fw, interface, status, rd(se, kSeIos), rd(gGcc.va, cmd), rd(gGcc.va, cfg),
           dfs, rd(se, kSeClkSelect), inheritedMclk, inheritedScl);
    // Wrap0 SE0 shares the bus with the USB audio codec. Do not take over an active command or reload firmware.
    if (((fw >> 8) & 0xffu) != GENI_PROTO_I2C || (interface & 1u) || (status & kSeActive)) return false;
    unsigned clockSelect = 0;
    if (dfs & 1u) {
        // Linux selects a firmware DFS table entry via GENI_CLK_SEL when DFS is active.
        while (clockSelect < kRcgDfsLevels &&
               (rd(gGcc.va, cmd + kRcgDfsPerfConfig + 4 * clockSelect) & kRcgRateMask) != kRcgTcxoDiv1)
            ++clockSelect;
        if (clockSelect == kRcgDfsLevels) {
            // Linux decodes a zero HID divider as divide-by-one. Only accept an all-zero
            // rate entry when firmware was already selecting that level.
            const uint32_t inherited = rd(se, kSeClkSelect);
            if (inherited >= kRcgDfsLevels ||
                (rd(gGcc.va, cmd + kRcgDfsPerfConfig + 4 * inherited) & kRcgRateMask) != 0) return false;
            clockSelect = inherited;
        }
    } else {
        if ((rd(gGcc.va, cfg) & kRcgRateMask) != kRcgTcxoDiv1) {
            wr(gGcc.va, cfg, (rd(gGcc.va, cfg) & ~kRcgRateMask) | kRcgTcxoDiv1);
            wr(gGcc.va, cmd, rd(gGcc.va, cmd) | kRcgUpdate);
        }
        for (unsigned us = 0; (rd(gGcc.va, cmd) & kRcgUpdate) && us < 2000; ++us) IODelay(1);
        if ((rd(gGcc.va, cmd) & (kRcgUpdate | kRcgRootOff)) ||
            (rd(gGcc.va, cfg) & kRcgRateMask) != kRcgTcxoDiv1) return false;
    }

    // qcom-geni-se.c's I/O defaults; GeniI2c prepares the main sequencer's FIFO and IRQ mask next.
    wr(se, kSeCgc, rd(se, kSeCgc) | 0x7f);
    wr(se, kSeDmaGeneral, rd(se, kSeDmaGeneral) | 0xf);
    wr(se, kSeOutput, 0x7f);
    wr(se, kSeForceDefault, 1);
    wr(se, kSeGsiEvent, 0);
    wr(se, kSeClkSelect, clockSelect);
    const uint32_t timing = inheritedMclk == kMclk && inheritedScl == kSclFirmware ? kSclFirmware : kSclStock;
    wr(se, GENI_CLK_CFG, kMclk);
    wr(se, GENI_SCL_COUNTERS, timing);
    const uint32_t mclk = rd(se, GENI_CLK_CFG), scl = rd(se, GENI_SCL_COUNTERS);
    printf("UKE BL: configured engine=%u rcg=%x/%x sel=%u mclk=%x scl=%x ios=%x\n",
           i, rd(gGcc.va, cmd), rd(gGcc.va, cfg), clockSelect, mclk, scl, rd(se, kSeIos));
    return rd(se, kSeClkSelect) == clockSelect && mclk == kMclk && scl == timing;
}

bool engineActive(unsigned i) {return (rd(gSe[i].va, kSeStatus) & kSeActive) != 0;}

// Interrupt context: read and clear the engine's status, keep it for the work loop and signal it. Nothing else.
void seIrq(void *context) {
    const unsigned index = (unsigned)(uintptr_t)context;
    EngineIo &io = gIo[index];
    const auto state = IOSimpleLockLockDisableInterrupt(io.lock);
    const uint32_t status = rd(io.va, GENI_M_IRQ_STATUS);
    // Mask delivery until the work loop feeds/drains the FIFO. Preserve the
    // firmware's watermark threshold throughout that deferred handoff.
    if (status) {
        io.deferred = true;
        wr(io.va, GENI_M_IRQ_EN, 0);
        if(!io.pending)io.firstPendingAt=mach_absolute_time();
        io.pending |= status;
        wr(io.va, GENI_M_IRQ_CLEAR, status);
    }
    IOSimpleLockUnlockEnableInterrupt(io.lock, state);
    if (!status) native_backlight_request[4] = native_backlight_request[4] + 1;
    native_backlight_request[5] = (int32_t)status;
    bump(10 + (int)index);
    thread_call_t call=__atomic_load_n(&gIrqCall,__ATOMIC_ACQUIRE);
    if(status) {
        __atomic_fetch_add(&native_backlight_signal[index][0],1u,__ATOMIC_RELAXED);
        if(call) {
            __atomic_fetch_add(&native_backlight_signal[index][1],1u,__ATOMIC_RELAXED);
            thread_call_enter(call);
            __atomic_fetch_add(&native_backlight_signal[index][2],1u,__ATOMIC_RELAXED);
        } else __atomic_fetch_add(&native_backlight_signal[index][3],1u,__ATOMIC_RELAXED);
    }
}
}  // namespace

class ShengBacklight : public IOService {
    OSDeclareDefaultStructors(ShengBacklight)
    // What one engine is doing: reading the level at start, then writing LSB and MSB of the newest level asked for.
    enum Step : uint8_t {StepNone, StepReadLsb, StepReadMsb, StepWriteLsb, StepWriteMsb, StepVerifyLsb, StepVerifyMsb,
                        StepReadInitialEnable, StepReadEnable, StepWriteEnable, StepVerifyEnable};
    struct Engine {GeniI2c bus; Step step; bool started, usable, combinedRead; uint8_t lsb, enableSending; int32_t written, effective, enable; uint32_t sending; uint64_t deadline; uint32_t retries, failedGoal;};
    Engine engine[2] = {};
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *fade = nullptr, *tick = nullptr, *attachTimer = nullptr, *request = nullptr;
    uint32_t level = 0, from = 0, to = 0, target = 0, attachTries = 0;
    uint64_t start = 0, period = 0;
    bool irqMode = false, published = false;
    bool attached[2] = {};
    uint32_t reportedLevel = 0;
    unsigned traceBudget=0, traceArmed=0;

    void traceFlow(const char *where) {
        printf("UKE BL flow: %s irq=%d/%d signal=%u/%u returned=%u/%u worker=%u tick=%u capW=%u/%u capT=%u/%u dirT=%u/%u waitMaxUs=%u/%u\n",
               where,native_backlight_stats[10],native_backlight_stats[11],
               __atomic_load_n(&native_backlight_signal[0][1],__ATOMIC_RELAXED),__atomic_load_n(&native_backlight_signal[1][1],__ATOMIC_RELAXED),
               __atomic_load_n(&native_backlight_signal[0][2],__ATOMIC_RELAXED),__atomic_load_n(&native_backlight_signal[1][2],__ATOMIC_RELAXED),
               native_backlight_flow[1][0],native_backlight_flow[2][0],
               native_backlight_flow[1][3],native_backlight_flow[1][4],native_backlight_flow[2][3],native_backlight_flow[2][4],
               native_backlight_flow[2][5],native_backlight_flow[2][6],
               native_backlight_irq_wait_us[0][1],native_backlight_irq_wait_us[1][1]);
    }

    static uint64_t now() {return mach_absolute_time();}
    static uint64_t after(uint32_t ms) {uint64_t delta = 0; nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &delta); return now() + delta;}
    int readRegister(Engine &e, uint8_t reg) {
        return e.combinedRead ? geni_i2c_start_read_combined(&e.bus, reg) : geni_i2c_start_read(&e.bus, reg);
    }

    // Starts the engine's next transfer when it is free.
    void kick(unsigned i) {
        Engine &e = engine[i];
        if (!e.usable || geni_i2c_busy(&e.bus) || engineActive(i)) return;
        int rc = 0;
        if (!e.started) {e.step = StepReadLsb; rc = readRegister(e, kBrtLsb);}
        else if (published && (e.effective != (int32_t)target || e.enable < 0 ||
                 bool(e.enable & kBlEnableBit) != bool(target)) && (e.failedGoal != target || e.retries < 3)) {
            if (e.failedGoal != target) e.retries = 0;
            e.sending = target;
            // Screen off only gates the outputs. A wake at the retained code
            // needs no brightness writes either; only verify/set BL_EN.
            if (!target || e.written==(int32_t)target) {
                e.step = StepReadEnable; rc = readRegister(e, kBlEnable);
            } else {
                e.step = StepWriteLsb;
                rc = geni_i2c_start_write(&e.bus, kBrtLsb, (uint8_t)(target & 7));
                if (!rc) {
                    // The old code may be partially overwritten until both bytes verify.
                    e.written = -1;
                    native_backlight_engine[i][0] = -1;
                    if (e.enable < 0 || (e.enable & kBlEnableBit)) e.effective = -1;
                }
            }
        }
        else {e.step = StepNone; return;}
        if (rc) {e.step = StepNone; return;}
        e.deadline = after(kStepDeadlineMs);
    }
    void noteFailure(unsigned i) {
        const Engine &e = engine[i];
        bump(5);
        native_backlight_engine[i][2] = native_backlight_engine[i][2] + 1;
        native_backlight_stats[6] = (int32_t)e.bus.error_status;
        native_backlight_stats[9] = (int32_t)(i << 8 | e.bus.error);
        native_backlight_stats[13] = (int32_t)(engine[0].bus.timeouts + engine[1].bus.timeouts);
        if (native_backlight_engine[i][2] <= 3)
            printf("UKE BL: engine=%u step=%u error=%u status=%x timeout=%u attempts=%u\n",
                   i, e.step, e.bus.error, e.bus.error_status, e.bus.timeouts, e.retries);
    }
    void completeLevel(unsigned i) {
        Engine &e = engine[i];
        e.effective = (int32_t)e.sending; e.retries = 0;
        native_backlight_engine[i][0] = e.written;
        static unsigned logged;
        if (i == 1 && (!e.sending || logged++ < 16 || !(logged % 64)))
            printf("UKE BL: req=%d target=%u effective=%d/%d code=%d/%d read=%d/%d enable=%x/%x err=%d/%d tmo=%d\n",
                   native_backlight_stats[1], target, engine[0].effective,engine[1].effective,engine[0].written, engine[1].written,
                   native_backlight_engine[0][1], native_backlight_engine[1][1], engine[0].enable, engine[1].enable,
                   native_backlight_engine[0][2], native_backlight_engine[1][2], native_backlight_stats[13]);
        if (engine[0].effective == (int32_t)target && engine[1].effective == (int32_t)target &&
            engine[0].enable >= 0 && engine[1].enable >= 0 &&
            bool(engine[0].enable & kBlEnableBit) == bool(target) && bool(engine[1].enable & kBlEnableBit) == bool(target))
            native_backlight_stats[2] = (int32_t)target;
    }
    // A transfer ended: the next step of the engine's sequence.
    void finished(unsigned i, int result) {
        Engine &e = engine[i];
        const bool ok = result == GeniDone;
        if (!ok) noteFailure(i);
        else bump(15);
        if (!ok && !e.started && e.bus.error == GeniErrTimeout) {
            // An unacknowledged abort must not be followed by a new M_CMD.
            if (engineActive(i)) e.retries = 2;
            else if (!e.combinedRead) {
                e.combinedRead = true;
                printf("UKE BL: retry engine=%u with combined register read\n", i);
            }
        }
        int rc = 0;
        switch (e.step) {
        case StepReadLsb:
            if (ok) {e.lsb = e.bus.data; rc = readRegister(e, kBrtMsb); e.step = StepReadMsb; break;}
            if (++e.retries < 3) {e.step = StepNone; kick(i); return;}
            e.started = true; e.written = -1; native_backlight_stats[3 + i] = -1;
            break;
        case StepReadMsb:
            if (!ok && ++e.retries < 3) {e.step = StepNone; kick(i); return;}
            e.written = ok ? (int32_t)(((uint32_t)e.bus.data << 3) | (e.lsb & 7)) : -1;
            native_backlight_stats[3 + i] = e.written;
            if (ok) {rc = readRegister(e, kBlEnable); e.step = StepReadInitialEnable; break;}
            e.started = true;
            break;
        case StepReadInitialEnable:
            if (!ok && ++e.retries < 3) {e.step = StepNone; kick(i); return;}
            e.started = true; e.enable = ok ? e.bus.data : -1;
            e.effective=ok?((e.enable & kBlEnableBit)?e.written:0):-1;
            if (ok) e.retries = 0;
            else {e.written = -1; native_backlight_stats[3 + i] = -1;}
            printf("UKE BL: adopted engine=%u brightness=%d enable=%x\n", i, e.written, e.enable);
            break;
        case StepWriteLsb:
            if (ok) {rc = geni_i2c_start_write(&e.bus, kBrtMsb, (uint8_t)((e.sending >> 3) & 0xff)); e.step = StepWriteMsb; break;}
            break;
        case StepWriteMsb:
            if (ok) {rc = readRegister(e, kBrtLsb); e.step = StepVerifyLsb; break;}
            break;
        case StepVerifyLsb:
            if (ok) {e.lsb = e.bus.data; rc = readRegister(e, kBrtMsb); e.step = StepVerifyMsb; break;}
            break;
        case StepVerifyMsb:
            if (ok) {
                const uint32_t read = ((uint32_t)e.bus.data << 3) | (e.lsb & 7);
                native_backlight_engine[i][1] = (int32_t)read;
                if (read == e.sending) {
                    e.written=(int32_t)read;
                    native_backlight_engine[i][0]=e.written;
                    if (e.sending && (e.enable < 0 || !(e.enable & kBlEnableBit))) {
                        // A newer off request must not relight an obsolete transfer.
                        if (!target) {e.step = StepNone; kick(i); return;}
                        rc = readRegister(e, kBlEnable); e.step = StepReadEnable; break;
                    }
                    completeLevel(i);
                } else {native_backlight_engine[i][2] = native_backlight_engine[i][2] + 1; ++e.retries; e.failedGoal = e.sending;}
            }
            break;
        case StepReadEnable:
            if (ok) {
                e.enable = e.bus.data;
                if (e.sending && !target) {e.step = StepNone; kick(i); return;}
                e.enableSending = (uint8_t)((e.enable & ~kBlEnableBit) | (e.sending ? kBlEnableBit : 0));
                if (e.enable != e.enableSending) {
                    rc = geni_i2c_start_write(&e.bus, kBlEnable, e.enableSending); e.step = StepWriteEnable; break;
                }
                completeLevel(i);
            }
            break;
        case StepWriteEnable:
            // Until readback succeeds, the output enable state is unknown.
            e.enable = -1;
            if (ok) {rc = readRegister(e, kBlEnable); e.step = StepVerifyEnable;}
            break;
        case StepVerifyEnable:
            if (ok && e.bus.data == e.enableSending) {
                e.enable = e.bus.data;
                printf("UKE BL: gate engine=%u on=%u enable=%x brightness=%u\n", i,
                       (unsigned)bool(e.enable & kBlEnableBit), e.enable, e.sending);
                traceFlow("gate");
                completeLevel(i);
            } else if (ok) {
                e.enable = -1; e.failedGoal = e.sending; ++e.retries;
                native_backlight_engine[i][2] = native_backlight_engine[i][2] + 1;
            }
            break;
        default:
            break;
        }
        if (rc) noteFailure(i);
        if ((!ok || rc) && e.started && e.step != StepReadLsb && e.step != StepReadMsb && e.step != StepReadInitialEnable) {
            e.failedGoal = e.sending; ++e.retries;
        }
        native_backlight_engine[i][3] = (int32_t)e.retries;
        if (geni_i2c_busy(&e.bus)) {e.deadline = after(kStepDeadlineMs); return;}
        e.step = StepNone;
        if (!published && engine[0].started && engine[1].started) {
            publishFirst();
            // Preserve both firmware levels until a real user/power request.
            return;
        }
        if (published) kick(i);
    }
    void advance(unsigned i, uint32_t status) {
        Engine &e = engine[i];
        if (!geni_i2c_busy(&e.bus)) return;
        const uint8_t phase = e.bus.phase;
        const uint32_t received = e.bus.rx_done, sent = e.bus.tx_done;
        const int result = geni_i2c_event(&e.bus, status);
        if (result != GeniPending) finished(i, result);
        else if (phase != e.bus.phase || received != e.bus.rx_done || sent != e.bus.tx_done)
            e.deadline = after(kStepDeadlineMs);
    }
    void armTick() {
        bool busy = false;
        uint64_t soonest = UINT64_MAX;
        for (const Engine &e : engine) if (geni_i2c_busy(&e.bus)) {busy = true; if (e.deadline < soonest) soonest = e.deadline;}
        if (!busy) {tick->cancelTimeout(); return;}
        if (!irqMode) {tick->setTimeoutMS(kPollMs); return;}
        tick->wakeAtTime((AbsoluteTime)soonest);
    }
    // The work loop's view of the engines: what the interrupts gathered, in polling mode the status register itself, then deadlines.
    void service(unsigned via=0) {
        native_backlight_flow[via][0]=native_backlight_flow[via][0]+1;
        for (unsigned i = 0; i < 2; ++i) {
            Engine &e = engine[i];
            const bool expired = irqMode && geni_i2c_busy(&e.bus) && now() >= e.deadline;
            bool watchdog = false;
            uint32_t captured=0;
            uint64_t capturedAt=0,waitNs=0;
            const uint32_t status = takeStatus(i, &watchdog,&captured,&capturedAt);
            if(status)native_backlight_flow[via][1+i]=native_backlight_flow[via][1+i]+1;
            if(captured)native_backlight_flow[via][3+i]=native_backlight_flow[via][3+i]+1;
            if(watchdog)native_backlight_flow[via][5+i]=native_backlight_flow[via][5+i]+1;
            if(expired)native_backlight_flow[via][7]=native_backlight_flow[via][7]+1;
            if(capturedAt) {
                absolutetime_to_nanoseconds(now()-capturedAt,&waitNs);
                const uint32_t us=(uint32_t)(waitNs/NSEC_PER_USEC);
                native_backlight_irq_wait_us[i][0]=us;
                if(us>native_backlight_irq_wait_us[i][1])native_backlight_irq_wait_us[i][1]=us;
            }
            const unsigned beforeStep=e.step,beforePhase=e.bus.phase;
            if (expired && watchdog) {
                const uint32_t count = native_backlight_irq_watchdog[i] + 1;
                native_backlight_irq_watchdog[i] = count;
                if (count <= 3)
                    printf("UKE BL: irq watchdog engine=%u recovered=%x count=%u\n", i, status, count);
            }
            if (status) advance(i, status);
            if(status && traceBudget) {
                --traceBudget;
                printf("UKE BL step: eng=%u via=%u step=%u>%u phase=%u>%u status=%x captured=%x waitUs=%llu expired=%u\n",
                       i,via,beforeStep,(unsigned)e.step,beforePhase,(unsigned)e.bus.phase,status,captured,
                       (unsigned long long)(waitNs/NSEC_PER_USEC),(unsigned)expired);
            }
            if (geni_i2c_busy(&e.bus) && now() >= e.deadline) {
                if (!e.bus.timeouts) {
                    volatile uint32_t *const se = gSe[i].va;
                    printf("UKE BL: timeout snapshot engine=%u step=%u phase=%u cmd=%x status=%x ios=%x irq=%x tx=%x rx=%x\n",
                           i, e.step, e.bus.phase, rd(se, GENI_M_CMD0), rd(se, kSeStatus), rd(se, kSeIos),
                           rd(se, GENI_M_IRQ_STATUS), rd(se, GENI_TX_FIFO_STATUS), rd(se, GENI_RX_FIFO_STATUS));
                    const uint32_t rcg = kRcgCommand[i];
                    printf("UKE BL: timeout clocks engine=%u mode=%s rcg=%x/%x dfs=%x sel=%x mclk=%x scl=%x irqen=%x wm=%x\n",
                           i, irqMode ? "irq" : "poll", rd(gGcc.va, rcg), rd(gGcc.va, rcg + kRcgConfig),
                           rd(gGcc.va, rcg + kRcgDfsCommand), rd(se, kSeClkSelect), rd(se, GENI_CLK_CFG),
                           rd(se, GENI_SCL_COUNTERS), rd(se, GENI_M_IRQ_EN), rd(se, GENI_TX_WATERMARK));
                }
                const int result = geni_i2c_timeout(&e.bus);
                native_backlight_stats[13] = (int32_t)(engine[0].bus.timeouts + engine[1].bus.timeouts);
                if (result != GeniPending) finished(i, result);
                else e.deadline = after(kStepDeadlineMs);
            }
            rearmStatus(i);
        }
        armTick();
    }
    static void deliverIrq(thread_call_param_t owner,thread_call_param_t) {
        auto *self=static_cast<ShengBacklight *>(owner);
        // Same IRQ -> high-priority callout -> runAction path already used by
        // the owned GOP fence backend. Coalesced IRQs are consumed under one
        // work-loop gate; the callback never drives I2C on the primary IRQ.
        self->loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<ShengBacklight *>(owner)->service(1);
            return kIOReturnSuccess;
        },self);
    }
    // A brightness request on the work loop (setProperties arms this timer at once): the newest one asked for, if not yet taken.
    static void requestFired(OSObject *owner, IOTimerEventSource *) {
        ShengBacklight *self = static_cast<ShengBacklight *>(owner);
        if (!self) return;
        if (!self->published) {
            if (native_backlight_stats[0] >= 0) self->request->setTimeoutMS(1);
            return;
        }
        native_backlight_request[2] = native_backlight_request[2] + 1;
        const uint64_t request = __atomic_exchange_n(&gRequest, 0ull, __ATOMIC_ACQUIRE);
        if (!(request >> 63)) return;
        const unsigned received = (unsigned)native_backlight_request[3] + 1;
        native_backlight_request[3] = (int32_t)received;
        const uint32_t goal = (uint32_t)(request >> 16) & 0xffff;
        const bool outputChanges = (goal == 0) != (self->target == 0);
        if (outputChanges && self->traceArmed < 4) {
            ++self->traceArmed;
            self->traceBudget = 48;
        }
        if (outputChanges || received <= 12 || !(received % 64))
            printf("UKE BL: apply request=%u goal=%u mode=%s\n",
                   received, goal, self->irqMode ? "irq" : "poll");

        // CoreBrightness owns the fade and the wake level. A zero target
        // disables the outputs while keeping the hardware brightness code.
        self->beginFade(goal, (uint32_t)request & 0xffff);
        if (goal && goal != self->reportedLevel) {
            self->reportedLevel = goal;
            self->publish(goal);
        }
    }
    static void tickFired(OSObject *owner, IOTimerEventSource *) {
        if (ShengBacklight *self = static_cast<ShengBacklight *>(owner)) self->service(2);
    }
    // Attaching the two lines, retried while the GIC driver is not up yet; the engines keep working by polling meanwhile.
    static void attachFired(OSObject *owner, IOTimerEventSource *sender) {
        ShengBacklight *self = static_cast<ShengBacklight *>(owner);   // see impersonate()
        if (!self || self->irqMode) return;
        if (!armcpu_spi_attach) {native_backlight_stats[12] = -1; return;}
        native_backlight_stats[14] = (int32_t)++self->attachTries;
        int rc = 0;
        for (unsigned i = 0; i < 2; ++i) {
            if (self->attached[i]) continue;
            rc = armcpu_spi_attach(32 + kSpi[i], false, seIrq, (void *)(uintptr_t)i, nullptr, nullptr);
            if (!rc) self->attached[i] = true;
            else break;
        }
        if (rc) {
            native_backlight_stats[12] = rc;
            if (rc == kIOReturnNotReady && self->attachTries < kAttachTries) sender->setTimeoutMS(kAttachMs);
            return;
        }
        self->irqMode = true;
        native_backlight_stats[12] = 1;
        printf("UKE BL: GENI interrupts attached mode=irq intid=%u/%u\n", 32 + kSpi[0], 32 + kSpi[1]);
        self->service(3);
    }

    void setLevel(uint32_t value) {
        target = value;
        for (Engine &e : engine) if (e.failedGoal != target) e.retries = 0;
        kick(0); kick(1);
        armTick();
    }
    static void fadeStep(OSObject *owner, IOTimerEventSource *sender) {
        ShengBacklight *self = static_cast<ShengBacklight *>(owner);   // see impersonate()
        if (!self) return;
        const uint64_t elapsed = now() - self->start;
        uint32_t next = self->to;
        if (elapsed < self->period)
            next = (uint32_t)((int64_t)self->from + ((int64_t)self->to - (int64_t)self->from) * (int64_t)elapsed / (int64_t)self->period);
        if (next != self->level || !self->period) {self->setLevel(next); self->level = next;}
        if (next != self->to) sender->setTimeoutMS(kStepMs);
    }
    // On the work loop: a fade from the level now shown to goal.
    void beginFade(uint32_t goal, uint32_t milliseconds) {
        fade->cancelTimeout();
        if(!goal) {level=0;setLevel(0);return;}
        from = level;
        to = goal;
        start = now();
        nanoseconds_to_absolutetime((uint64_t)milliseconds * NSEC_PER_MSEC, &period);
        fadeStep(this, fade);
    }
    static IOReturn begin(OSObject *owner, void *, void *, void *, void *) {
        ShengBacklight *self = static_cast<ShengBacklight *>(owner);   // see impersonate()
        if (!self) return kIOReturnBadArgument;
        self->kick(0); self->kick(1);
        if (!self->published && self->engine[0].started && self->engine[1].started) self->publishFirst();
        self->armTick();
        return kIOReturnSuccess;
    }
    void publish(uint32_t value) {
        OSDictionary *brightness = OSDictionary::withCapacity(3), *milliNits = OSDictionary::withCapacity(3), *parameters = OSDictionary::withCapacity(2);
        OSNumber *min = OSNumber::withNumber(0ULL, 32), *max = OSNumber::withNumber(kMaxLevel, 32), *current = OSNumber::withNumber(value, 32);
        OSNumber *minMilli = OSNumber::withNumber(kMinNits * 1000ULL, 32), *maxMilli = OSNumber::withNumber(kMaxNits * 1000ULL, 32);
        OSNumber *currentMilli = OSNumber::withNumber((uint64_t)value * kMaxNits * 1000 / kMaxLevel, 32);
        if (brightness && milliNits && parameters && min && max && current && minMilli && maxMilli && currentMilli) {
            brightness->setObject("min", min); brightness->setObject("max", max); brightness->setObject("value", current);
            milliNits->setObject("min", minMilli); milliNits->setObject("max", maxMilli); milliNits->setObject("value", currentMilli);
            parameters->setObject("brightness", brightness);
            // BrightnessControl takes the nits range and the current nits from here on iPadOS 27.
            parameters->setObject("BrightnessMilliNits", milliNits);
            setProperty("IODisplayParameters", parameters);
        }
        OSSafeReleaseNULL(min); OSSafeReleaseNULL(max); OSSafeReleaseNULL(current); OSSafeReleaseNULL(minMilli); OSSafeReleaseNULL(maxMilli);
        OSSafeReleaseNULL(currentMilli); OSSafeReleaseNULL(brightness); OSSafeReleaseNULL(milliNits); OSSafeReleaseNULL(parameters);
    }
    // CoreBrightness's Display.m fails without a marketing table (equally spaced slider samples, signed 16.16 nits), and without calibration
    // parameters it reads that table before loading it. Both describe the same range as BrightnessMilliNits. BrightnessControl takes the
    // product luminances from their own properties, not from the calibration dictionary: at zero the table rescales to infinity and every
    // slider position means the maximum.
    void publishCalibration() {
        const int32_t table[] = {(int32_t)(kMinNits << 16), (int32_t)(kMidNits << 16), (int32_t)(kMaxNits << 16)};
        if (OSData *marketing = OSData::withBytes(table, sizeof table)) {
            setProperty("backlight-marketing-table", marketing);
            marketing->release();
        }
        // LmaxProduct is read as 4 bytes of 16.16. LmidProduct and LminProduct take a number as it is; as data they would be read as a float.
        if (OSData *value = OSData::withBytes(&table[2], sizeof table[2])) {
            setProperty("LmaxProduct", value);
            value->release();
        }
        const struct {const char *key; uint32_t nits;} products[] = {{"LminProduct", kMinNits}, {"LmidProduct", kMidNits}};
        for (const auto &product : products) {
            if (OSNumber *value = OSNumber::withNumber(product.nits, 32)) {
                setProperty(product.key, value);
                value->release();
            }
        }
        OSDictionary *calibration = OSDictionary::withCapacity(6);
        if (!calibration) return;
        const struct {const char *key; uint32_t nits;} fields[] = {{"lMinProduct", kMinNits}, {"lMidProduct", kMidNits}, {"lMaxProduct", kMaxNits},
                                                                   {"lMinPanel", kMinNits}, {"lMidPanel", kMidNits}, {"lMaxPanel", kMaxNits}};
        for (const auto &field : fields) {
            if (OSNumber *number = OSNumber::withNumber((uint64_t)field.nits << 16, 32)) {
                calibration->setObject(field.key, number);
                number->release();
            }
        }
        setProperty("backlight-calibration-parameters", calibration);
        calibration->release();
    }
    // The service presents itself as AppleARMBacklight (AppleARMPlatform's class, loaded but without a device on this board): its
    // vtable is copied and getMetaClass (slot 7) answers that metaclass. IOObjectConformsTo and the sandbox both go through it: the
    // sandbox decides iokit-set-properties by walking getMetaClass's superclass chain by name, and backboardd's profile lets it set
    // the brightness keys on AppleARMBacklight only (int-275: "deny iokit-set-properties iokit-class:ShengBacklight property:FadePeriod").
    // AppleARMBacklight and IOService stay in that chain, so OSDynamicCast to IOService works; to ShengBacklight it does not, which is
    // why the event source actions cast statically. No loaded kext looks this class up (AppleSPU, AppleEmbeddedLightSensor and
    // AppleM68Buttons do, and are not part of this kernel), and the service is never freed.
    static inline const OSMetaClass *gAppleClass = nullptr;
    static inline bool gImpersonating = false;
    static const OSMetaClass *appleMetaClass(const OSMetaClassBase *) {return gAppleClass;}
    bool impersonate() {
        enum {kSlots = 320, kGetMetaClass = 7};
        const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");
        gAppleClass = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
        OSSafeReleaseNULL(name);
        if (!gAppleClass) return false;
#if __has_feature(ptrauth_calls)
        // arm64e: the vtable pointer and its entries are signed, so a copied vtable would not authenticate. getMetaClass itself
        // answers the AppleARMBacklight metaclass once this flag is set (its definition below the class).
        __atomic_store_n(&gImpersonating, true, __ATOMIC_RELEASE);
        return true;
#endif
        uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));
        if (!copy) return false;
        memcpy(copy, *reinterpret_cast<uintptr_t **>(this), kSlots * sizeof(uintptr_t));
        copy[kGetMetaClass] = reinterpret_cast<uintptr_t>(&appleMetaClass);
        __asm__ volatile("dmb ish" ::: "memory");
        *reinterpret_cast<uintptr_t **>(this) = copy;
        return true;
    }
    // Both engines answered (or gave up) the level read: publish what the panel shows now and let CoreBrightness find the service.
    void publishFirst() {
        if (!ukeBacklightInitialReady(native_backlight_stats[3], native_backlight_stats[4])) {
            fail(4, native_backlight_stats[9]);
            return;
        }
        published = true;
        const int32_t read = native_backlight_stats[3] >= 0 ? native_backlight_stats[3] : native_backlight_stats[4];
        level = to = target = read >= 0 ? (uint32_t)read : kMaxLevel / 2;
        // CoreBrightness finds the backlight by backlight-control = true and takes it for the built-in display's only when
        // IOObjectConformsTo(service, "AppleARMBacklight") holds (CBDisplayContaineriOS findBacklight). Only for the built-in display
        // does CoreBrightness start the libEDR server (com.apple.libEDR.Notification) that QuartzCore's IOMFBServer sends to
        // synchronously once backlight-control makes CADeviceHasBacklight true. int-236 published backlight-control without the
        // conformance: the server never started, the send blocked with the display server lock held and the screen stopped updating.
        // sheng-backlight-control=0 leaves both out.
        uint32_t control = 1;
        PE_parse_boot_argn("sheng-backlight-control", &control, sizeof control);
        // backboardd's sandbox profile refuses FadePeriod on this service (int-276), and CoreBrightness sends it with every brightness:
        // the grant (patches/xnu/zzwb-native-sandbox-grant-set-properties.patch) lets backboardd alone set its properties, which
        // setProperties checks itself.
        const bool impersonated = control == 1 && impersonate() &&
            setProperty("NativeSandboxGrantProcess", "backboardd") && setProperty("backlight-control", true);
        publishCalibration();
        reportedLevel = level;
        publish(reportedLevel);
        registerService();
        native_backlight_stats[0] = impersonated ? 4 : 3;
        if (impersonated) __atomic_store_n(&gRuntimeReady, true, __ATOMIC_RELEASE);
        uint32_t interrupts = 1;
        PE_parse_boot_argn("uke-backlight-irq", &interrupts, sizeof interrupts);
        if (interrupts) attachTimer->setTimeoutMS(1);
        else printf("UKE BL: runtime polling enabled; GENI SPIs remain unattached\n");
        printf("UKE BL: ready st=%d initial=%d/%d proto=%x/%x\n", native_backlight_stats[0],
               native_backlight_stats[3], native_backlight_stats[4], native_backlight_stats[7], native_backlight_stats[8]);
    }
public:
    bool startBacklight(const bool usable[2]) {
        loop = IOWorkLoop::workLoop();
        fade = IOTimerEventSource::timerEventSource(this, &ShengBacklight::fadeStep);
        tick = IOTimerEventSource::timerEventSource(this, &ShengBacklight::tickFired);
        attachTimer = IOTimerEventSource::timerEventSource(this, &ShengBacklight::attachFired);
        request = IOTimerEventSource::timerEventSource(this, &ShengBacklight::requestFired);
        if (!loop || !fade || !tick || !attachTimer || !request) return false;
        if (loop->addEventSource(fade) || loop->addEventSource(tick) || loop->addEventSource(attachTimer) ||
            loop->addEventSource(request)) return false;
        for (unsigned i = 0; i < 2; ++i) {
            gIo[i].va = gSe[i].va;
            gIo[i].lock = IOSimpleLockAlloc();
            if (!gIo[i].lock) return false;
            const GeniIo io = {ioRead, ioWrite, &gIo[i]};
            uint32_t config = 0;
            engine[i].usable = usable[i] && geni_i2c_prepare(&engine[i].bus, &io, kAddress, &config) == 0;
            native_backlight_stats[7 + i] = (int32_t)config;
            engine[i].started = !engine[i].usable;
            engine[i].written = -1;
            if (!engine[i].usable) {
                native_backlight_stats[3 + i] = -1;
                continue;
            }
            // Initialize receive flow control for short register reads too, as qcom-geni-se does.
            const uint32_t depth = engine[i].bus.rx_fifo_words;
            wr(gSe[i].va, GENI_RX_WATERMARK, depth - 1);
            wr(gSe[i].va, GENI_RX_RFR_WATERMARK, depth);
        }
        if (!engine[0].usable || !engine[1].usable) return false;
        thread_call_t irqCall = thread_call_allocate_with_options(&ShengBacklight::deliverIrq, this,
            THREAD_CALL_PRIORITY_KERNEL_HIGH,THREAD_CALL_OPTIONS_ONCE);
        if (!irqCall) return false;
        // This board service and its IRQ bindings live for the boot. Pin the
        // owner before publishing a callout that the primary IRQ can queue.
        retain();
        __atomic_store_n(&gIrqCall,irqCall,__ATOMIC_RELEASE);
        printf("UKE BL flow: sources loop=%p irqCall=%p deadline=%p\n",loop,irqCall,tick);
        native_backlight_stats[0] = 2;
        loop->runAction(&ShengBacklight::begin, this);
        return true;
    }
    // Level for signed 16.16 nits, proportional as the LED current is; a lit panel never rounds down to off.
    static uint32_t levelForNits(int32_t fixed) {
        return ukeBacklightLevelForNits(fixed);
    }
    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);
        if (!dictionary) return kIOReturnBadArgument;
        // iPadOS 27 sends signed 16.16 nits and ramps them itself (zero or less is the display going dark); iPadOS 17 sends a level and a
        // fade period in 16.16 seconds.
        uint32_t goal;
        uint64_t milliseconds = 0;
        if (OSNumber *nits = OSDynamicCast(OSNumber, dictionary->getObject("brightness-nits"))) {
            goal = levelForNits((int32_t)nits->unsigned32BitValue());
        } else if (OSNumber *brightness = OSDynamicCast(OSNumber, dictionary->getObject("brightness"))) {
            OSNumber *fadePeriod = OSDynamicCast(OSNumber, dictionary->getObject("FadePeriod"));
            const int32_t requested = (int32_t)brightness->unsigned32BitValue();
            goal = requested < 0 ? 0 : (uint32_t)requested > kMaxLevel ? kMaxLevel : (uint32_t)requested;
            milliseconds = fadePeriod ? ((uint64_t)fadePeriod->unsigned32BitValue() * 1000) >> 16 : 0;
            if (milliseconds > 5000) milliseconds = 5000;
        } else {
            return kIOReturnSuccess;
        }
        bump(1);
        native_backlight_request[0] = (int32_t)goal;
        native_backlight_request[1] = (int32_t)milliseconds;
        // Handed to the work loop without waiting for it (a runAction here blocks backboardd's brightness thread on the work loop's
        // gate; int-277 got 53 requests in the first 41 s and none after): a newer request replaces one not yet taken. The request timer, not
        // the interrupt source, carries it: int-278 signalled the interrupt source for 345 requests and wrote none.
        __atomic_store_n(&gRequest, 1ull << 63 | (uint64_t)goal << 16 | milliseconds, __ATOMIC_RELEASE);
        request->setTimeoutUS(1);
        return kIOReturnSuccess;
    }
};
#if __has_feature(ptrauth_calls)
// OSDefineMetaClassAndStructors(ShengBacklight, IOService) with getMetaClass answering the AppleARMBacklight metaclass after impersonate().
ShengBacklight::MetaClass ShengBacklight::gMetaClass;
const OSMetaClass * const ShengBacklight::metaClass = &ShengBacklight::gMetaClass;
const OSMetaClass * const ShengBacklight::superClass = &IOService::gMetaClass;
ShengBacklight::ShengBacklight(const OSMetaClass *meta) : IOService(meta) {}
ShengBacklight::~ShengBacklight() {}
const OSMetaClass *ShengBacklight::getMetaClass() const
{
    return __atomic_load_n(&gImpersonating, __ATOMIC_ACQUIRE) && gAppleClass ? gAppleClass : &gMetaClass;
}
OSMetaClassConstructorInit(ShengBacklight, IOService, )
OSDefineDefaultStructors(ShengBacklight, IOService)
#else
OSDefineMetaClassAndStructors(ShengBacklight, IOService)
#endif

// Called by BacklightProbe.cpp once this file's classes are registered (its static initializer runs between preModLoad and postModLoad).
// The service starts with the engines' level reads in flight and publishes itself when they are answered.
extern "C" void sheng_backlight_start(void) {
    if (!mapWindow(gGcc, kGcc, 0x60000) || !mapWindow(gSe[0], kSeA, 0x4000) || !mapWindow(gSe[1], kSeB, 0x4000)) {fail(1, -12); return;}
    native_backlight_stats[0] = 1;
    const unsigned pins[] = {52, 53, 176, 177};
    for (unsigned i = 0; i < 4; ++i) {
        if (!mapWindow(gPins[i], 0x0f100000ull + 0x1000ull * pins[i], 0x1000)) {fail(1, -12); return;}
        // Both SDA and SCL use function 1, 2 mA and pull-up in uke-live.dts.
        uint32_t cfg = rd(gPins[i].va, 0);
        cfg &= ~((15u << 2) | (7u << 6) | (1u << 9) | 3u);
        cfg |= (1u << 2) | 3u;
        wr(gPins[i].va, 0, cfg);
    }
    for (unsigned i = 0; i < 2; ++i) wr(gGcc.va, kGccVote[i], rd(gGcc.va, kGccVote[i]) | kVoteBits[i]);
    for (uint32_t halt : kHalt) {
        // The AHB branches run under hardware clock gating (HW_CTL, bit 1 of the branch register): their CLK_OFF bit says off while the
        // bus is idle, so, as clk-branch.c does in that mode, they are not waited for (int-231 failed here on wrap 1 s_ahb, 0x23280).
        if (rd(gGcc.va, halt) & (1u << 1)) continue;
        unsigned us = 0;
        while ((rd(gGcc.va, halt) & (1u << 31)) && us < 200) {IODelay(1); ++us;}
        if (us == 200) {fail(2, (int)halt); return;}
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!configureEngine(i)) {fail(6, (int)i); return;}
    }
    const bool usable[2] = {true, true};
    ShengBacklight *backlight = OSTypeAlloc(ShengBacklight);
    if (!backlight || !backlight->init() || !backlight->attach(IOService::getPlatform()) || !backlight->startBacklight(usable)) {
        if (backlight) backlight->release();
        fail(3, -19); return;
    }
    backlight->release();
}

extern "C" bool native_backlight_runtime_ready(void) {
    return __atomic_load_n(&gRuntimeReady, __ATOMIC_ACQUIRE);
}
