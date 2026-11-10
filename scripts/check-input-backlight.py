#!/usr/bin/env python3
"""Run production button policies and backlight transfer methods under ASan/UBSan."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def method(source, name, result='void'):
    start = source.index(f'    {result} {name}(')
    at = source.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def irq_harness():
    source = (ROOT / 'package/ButtonProbe.cpp').read_text()
    start = source.index('void buttonsIrq(')
    end = source.index('\nbool sample(', start)
    code = source[start:end].replace('__asm__ volatile("dsb sy" ::: "memory");', '__atomic_thread_fence(__ATOMIC_SEQ_CST);')
    return r'''
namespace ButtonIRQ {
constexpr uint32_t kAccEnable = 0x100, kIrqStatus = 0x104, kIrqClear = 0x108;
constexpr uint32_t kPdcCfg = 0x110, kPdcPin = 1;
constexpr uint8_t kPowerBit = 0x80, kResinBit = 0x40, kGpioBit = 1;
struct Window {uint32_t *va;};
uint32_t intr[1], pdc[128], channels[8][128];
Window gIntr = {intr}, gPdc = {pdc};
uint32_t gApids = 7, gEvents, gPending[2];
int gPon = 2, gGpio = 5;
int32_t native_buttons_stats[10];
using event_t = void *;
void thread_wakeup(event_t) {}
uint32_t *apidReg(int apid, uint32_t reg) {CHECK(apid >= 0 && apid < 7); return &channels[apid][reg / 4];}
IRQ_CODE
void test() {
    channels[2][kAccEnable / 4] = channels[5][kAccEnable / 4] = channels[6][kAccEnable / 4] = 1;
    channels[2][kIrqStatus / 4] = 0xc0; channels[5][kIrqStatus / 4] = 1;
    intr[0] = (1u << 2) | (1u << 5) | (1u << 6) | (1u << 31);
    buttonsIrq(nullptr);
    CHECK(!channels[2][kAccEnable / 4] && !channels[5][kAccEnable / 4]);
    CHECK(channels[6][kAccEnable / 4] == 1 && channels[6][kIrqClear / 4] == 0xff);
    CHECK(gPending[0] == 0xc0 && gPending[1] == 1 && gEvents == 1);
    CHECK(native_buttons_stats[1] == 1 && native_buttons_stats[6] == 1);
    intr[0] = 1u << 6;
    for (unsigned i = 0; i < 1000; ++i) buttonsIrq(nullptr);
    CHECK(pdc[(kPdcCfg + kPdcPin * 4) / 4] == 4 && native_buttons_stats[0] == -9);
    CHECK(gEvents == 1 && native_buttons_stats[9] == 1000);
}
}
'''.replace('IRQ_CODE', code)


HARNESS = r'''
#include <cstdint>
#include "UkeButtonState.h"
#include "UkeBacklightState.h"
volatile int32_t native_backlight_stats[16], native_backlight_engine[2][4];
static constexpr uint8_t kBrtLsb = 4, kBrtMsb = 5, kBlEnable = 8, kBlEnableBit = 0x40;
static constexpr uint32_t kStepDeadlineMs = 25;
static uint64_t after(uint32_t ms) {return ms;}
static void bump(unsigned i) {native_backlight_stats[i] = native_backlight_stats[i] + 1;}
struct Driver {
    enum Step : uint8_t {StepNone, StepReadLsb, StepReadMsb, StepWriteLsb, StepWriteMsb, StepVerifyLsb, StepVerifyMsb, StepReadInitialEnable, StepReadEnable, StepWriteEnable, StepVerifyEnable};
    struct Engine {GeniI2c bus; Step step; bool started, usable, combinedRead; uint8_t lsb, enableSending; int32_t written, effective, enable; uint32_t sending;
                   uint64_t deadline; uint32_t retries, failedGoal;};
    Engine engine[2] = {};
    bool published = false;
    uint32_t target = 0;
    void publishFirst() {
        INITIAL_GUARD
        published = true; target = (uint32_t)engine[0].written;
    }
    void traceFlow(const char *) {}
    METHODS
};
static void setup(Driver &d, Engine *models) {
    for (unsigned i = 0; i < 2; ++i) {
        boot_state(&models[i]); models[i].slave[4] = 0; models[i].slave[5] = 64; models[i].slave[8] = 0x7f;
        GeniIo io = {eng_read, eng_write, &models[i]};
        uint32_t config;
        CHECK(geni_i2c_prepare(&d.engine[i].bus, &io, 0x11, &config) == 0);
        d.engine[i].usable = true; d.engine[i].written = -1;
    }
}
static int failStep = -1, failEngine = 1;
static bool badEnableReadback;
static void drain(Driver &d, Engine *models) {
    for (unsigned tick = 0; tick < 256; ++tick) {
        bool busy = false;
        for (unsigned i = 0; i < 2; ++i) {
            auto &e = d.engine[i];
            if (!geni_i2c_busy(&e.bus)) continue;
            busy = true;
            if (e.step == Driver::StepWriteLsb) CHECK(e.sending != 0);
            if (e.step == Driver::StepWriteEnable && e.sending) {
                CHECK((((unsigned)models[i].slave[5] << 3) | (models[i].slave[4] & 7)) == e.sending);
            }
            const int present = models[i].present;
            const bool forced = (int)i == failEngine && (int)e.step == failStep;
            if (forced) models[i].present = 0;
            const unsigned oldEnable = models[i].slave[8];
            int result = geni_i2c_poll(&e.bus);
            models[i].present = present;
            CHECK(!((oldEnable ^ models[i].slave[8]) & ~kBlEnableBit));
            if (result == GeniDone && badEnableReadback && e.step == Driver::StepVerifyEnable && (int)i == failEngine)
                e.bus.data ^= 1;
            if (result == GeniPending && (models[i].mute || (models[i].fail_split_read && models[i].active && models[i].op == GENI_OP_READ)))
                result = geni_i2c_timeout(&e.bus);
            if (result != GeniPending) d.finished(i, result);
        }
        if (!busy) return;
    }
    CHECK(false && "transfer did not terminate");
}
int main() {
    CHECK(upstream_geni_tests() == 0);
    {
        Driver driver; Engine models[2]; setup(driver, models);
        models[0].fail_split_read = 1;
        driver.kick(0); driver.kick(1); drain(driver, models);
        CHECK(driver.published && driver.engine[0].combinedRead && !driver.engine[1].combinedRead);
        CHECK(driver.engine[0].written == 512 && driver.engine[1].written == 512);
    }
    {
        Driver driver; Engine models[2]; setup(driver, models);
        models[0].mute = 1; models[0].cancel_ack = 0; models[0].abort_ack = 0;
        driver.kick(0); driver.kick(1); drain(driver, models);
        CHECK(!driver.published && models[0].commands == 1 && !driver.engine[0].combinedRead);
    }
    ButtonIRQ::test();
    CHECK(ukeButtonOwned(0x01001300, 0, 0x13, 0));
    CHECK(!ukeButtonOwned(0x00001300, 0, 0x13, 0));
    CHECK(!ukeButtonOwned(0x01001300, 1, 0x13, 0));
    CHECK(!ukeButtonOwned(0x01018d00, 0, 0x13, 0));
    for (unsigned bits = 0; bits < 8; ++bits) {
        UkeButtonState s = {};
        const uint8_t pon = ((bits & 1) ? 0x80 : 0) | ((bits & 4) ? 0x40 : 0);
        const uint8_t gpio = (bits & 2) ? 0 : 1;
        const unsigned expected = ((bits & 1) ? 2 : 0) | ((bits & 2) ? 4 : 0) | ((bits & 4) ? 8 : 0);
        ukeButtonSample(s, true, pon, true, gpio, 100);
        CHECK((s.mask & 10) == (expected & 10));
        ukeButtonSample(s, true, pon, true, gpio, 131); CHECK(!(s.mask & 4));
        ukeButtonSample(s, true, pon, true, gpio, 132); CHECK(s.mask == expected);
        ukeButtonSample(s, false, 0, false, 1, 140); CHECK(s.mask == expected);
        ukeButtonSample(s, true, 0, true, 1, 150);
        ukeButtonSample(s, true, 0, true, 1, 182); CHECK(s.mask == 0);
    }
    UkeButtonState bounce = {};
    ukeButtonSample(bounce, true, 0, true, 0, 0);
    ukeButtonSample(bounce, true, 0, true, 1, 20);
    ukeButtonSample(bounce, true, 0, true, 0, 25);
    ukeButtonSample(bounce, true, 0, true, 0, 56); CHECK(!bounce.mask);
    ukeButtonSample(bounce, true, 0, true, 0, 57); CHECK(bounce.mask == 4);
    CHECK(ukeBacklightLevelForNits(-1) == 0 && ukeBacklightLevelForNits(0) == 0);
    CHECK(ukeBacklightLevelForNits(1) == 1 && ukeBacklightLevelForNits(500 << 16) == 2047);
    CHECK(ukeBacklightLevelForNits(INT32_MAX) == 2047);
    uint32_t previous = 0;
    for (int n = 0; n <= 500; ++n) {const auto level = ukeBacklightLevelForNits(n << 16); CHECK(level >= previous); previous = level;}
    Driver d; Engine models[2]; setup(d, models);
    d.kick(0); d.kick(1); drain(d, models);
    CHECK(d.published && models[0].slave[5] == 64 && models[1].slave[5] == 64);
    CHECK(d.engine[0].written == 512 && d.engine[1].written == 512);
    for (unsigned target : {0u, 1u, 1024u, 2047u}) {
        d.target = target; d.kick(0); d.kick(1); drain(d, models);
        for (unsigned i = 0; i < 2; ++i) {
            const unsigned code = target ? target : 512;
            CHECK(d.engine[i].effective == (int)target && d.engine[i].written == (int)code);
            CHECK((((unsigned)models[i].slave[5] << 3) | models[i].slave[4]) == code);
            CHECK(bool(models[i].slave[8] & kBlEnableBit) == bool(target));
        }
    }
    // Preserve every non-enable bit for all possible firmware BL_EN values.
    for (unsigned reg = 0; reg < 256; ++reg) {
        Driver driver; Engine pair[2]; setup(driver, pair);
        pair[0].slave[8] = reg; pair[1].slave[8] = reg ^ 0x80;
        driver.kick(0); driver.kick(1); drain(driver, pair);
        CHECK(driver.published && pair[0].slave[8] == reg && pair[1].slave[8] == (reg ^ 0x80));
        driver.target = 1024; driver.kick(0); driver.kick(1); drain(driver, pair);
        CHECK(pair[0].slave[8] == (reg | 0x40) && pair[1].slave[8] == ((reg ^ 0x80) | 0x40));
        driver.target = 0; driver.kick(0); driver.kick(1); drain(driver, pair);
        CHECK(pair[0].slave[8] == (reg & ~0x40) && pair[1].slave[8] == ((reg ^ 0x80) & ~0x40));
        for (unsigned i = 0; i < 2; ++i) {
            CHECK(driver.engine[i].effective == 0 && driver.engine[i].written == 1024);
            CHECK(pair[i].slave[5] == 128 && pair[i].slave[4] == 0);
        }
        driver.target = 1024; driver.kick(0); driver.kick(1);
        CHECK(driver.engine[0].step == Driver::StepReadEnable && driver.engine[1].step == Driver::StepReadEnable);
        drain(driver, pair);
        CHECK(driver.engine[0].effective == 1024 && driver.engine[1].effective == 1024);
        driver.target = 0; driver.kick(0); driver.kick(1); drain(driver, pair);
        driver.target = 913; driver.kick(0); driver.kick(1); drain(driver, pair);
        CHECK(pair[0].slave[8] == (reg | 0x40) && pair[1].slave[8] == ((reg ^ 0x80) | 0x40));
        CHECK(driver.engine[0].written == 913 && driver.engine[1].written == 913);
    }
    // Read, write and verification errors remain bounded on either controller.
    for (int which : {0, 1}) for (int step : {Driver::StepReadEnable, Driver::StepWriteEnable, Driver::StepVerifyEnable}) {
        Driver driver; Engine pair[2]; setup(driver, pair);
        driver.kick(0); driver.kick(1); drain(driver, pair);
        failEngine = which; failStep = step;
        driver.target = 0; driver.kick(0); driver.kick(1); drain(driver, pair);
        if (step == Driver::StepVerifyEnable) {
            // A failed verify after a successful disable is recovered by a fresh
            // read proving BL_EN is already clear, rather than blindly retrying.
            CHECK(driver.engine[which].effective == 0 && driver.engine[which].written == 512 && driver.engine[which].enable == 0x3f);
        } else CHECK(driver.engine[which].retries == 3 && driver.engine[which].written == 512);
        CHECK(driver.engine[1-which].effective == 0 && driver.engine[1-which].written == 512);
        const unsigned commands = pair[which].commands; driver.kick(which); CHECK(commands == pair[which].commands);
        failStep = -1; driver.target = 701; driver.kick(0); driver.kick(1); drain(driver, pair);
        CHECK(driver.engine[0].written == 701 && driver.engine[1].written == 701);
    }
    {
        Driver driver; Engine pair[2]; setup(driver, pair);
        driver.kick(0); driver.kick(1); drain(driver, pair);
        badEnableReadback = true; driver.target = 0; driver.kick(0); driver.kick(1); drain(driver, pair);
        CHECK(driver.engine[failEngine].effective == 0 && driver.engine[failEngine].written == 512 && driver.engine[failEngine].enable == 0x3f);
        badEnableReadback = false; driver.target = 1000; driver.kick(0); driver.kick(1); drain(driver, pair);
        CHECK(driver.engine[0].written == 1000 && driver.engine[1].written == 1000);
    }
    for (int which : {0, 1}) {
        Driver driver; Engine pair[2]; setup(driver, pair);
        failEngine = which; failStep = Driver::StepReadInitialEnable;
        driver.kick(0); driver.kick(1); drain(driver, pair);
        CHECK(!driver.published && driver.engine[which].written == -1 && driver.engine[which].retries == 3);
        failStep = -1;
    }
    // Supersede a wake before each enable-related stage; the newest off must win.
    for (int stop : {Driver::StepVerifyMsb, Driver::StepReadEnable, Driver::StepWriteEnable}) {
        Driver driver; Engine pair[2]; setup(driver, pair);
        driver.kick(0); driver.kick(1); drain(driver, pair);
        driver.target = 0; driver.kick(0); driver.kick(1); drain(driver, pair);
        driver.target = 1600; driver.kick(0); driver.kick(1);
        for (unsigned tick = 0; (int)driver.engine[0].step != stop && tick < 128; ++tick) {
            int result = geni_i2c_poll(&driver.engine[0].bus);
            if (result != GeniPending) driver.finished(0, result);
        }
        CHECK((int)driver.engine[0].step == stop);
        driver.target = 0; driver.kick(0); driver.kick(1); drain(driver, pair);
        CHECK(driver.engine[0].effective == 0 && driver.engine[1].effective == 0);
        CHECK(!(pair[0].slave[8] & 0x40) && !(pair[1].slave[8] & 0x40));
        CHECK(pair[0].slave[5] == 200 && pair[1].slave[5] == 200);
    }
    // One failed controller cannot mark both controllers successful or loop forever.
    d.target = 777; models[1].present = 0; d.kick(0); d.kick(1); drain(d, models);
    CHECK(d.engine[0].written == 777 && d.engine[1].written == -1);
    CHECK(d.engine[1].retries == 3 && !geni_i2c_busy(&d.engine[1].bus));
    const unsigned commands = models[1].commands;
    d.kick(1); CHECK(models[1].commands == commands);
    models[1].present = 1; d.target = 778; d.kick(0); d.kick(1); drain(d, models);
    CHECK(d.engine[0].written == 778 && d.engine[1].written == 778);
    // Cancel/abort a muted engine, then recover on a later user request.
    d.target = 900; models[0].mute = 1; d.kick(0); d.kick(1); drain(d, models);
    CHECK(d.engine[0].written == -1 && d.engine[1].written == 900);
    CHECK(d.engine[0].bus.timeouts && d.engine[0].retries == 3);
    models[0].mute = 0; d.target = 901; d.kick(0); d.kick(1); drain(d, models);
    CHECK(d.engine[0].written == 901 && d.engine[1].written == 901);
    CHECK(!ukeBacklightInitialReady(-1, -1) && !ukeBacklightInitialReady(0, -1));
    CHECK(!ukeBacklightInitialReady(2048, 1000) && ukeBacklightInitialReady(0, 2047));
    Driver failed; Engine absent[2]; setup(failed, absent);
    absent[0].present = absent[1].present = 0;
    failed.kick(0); failed.kick(1); drain(failed, absent);
    CHECK(!failed.published && native_backlight_stats[0] == -4);
    CHECK(failed.engine[0].written == -1 && failed.engine[1].written == -1);
    CHECK(failed.engine[0].retries == 3 && failed.engine[1].retries == 3);
    CHECK(!absent[0].slave[4] && absent[0].slave[5] == 64);
    CHECK(!absent[1].slave[4] && absent[1].slave[5] == 64);
    printf("PASS input/backlight production methods: %u checks\n", checks);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'package/AppleARMBacklight.cpp').read_text()
    model = (ROOT / 'package/tests/geni_i2c_host.c').read_text()
    model = model.replace('#include "../GeniI2c.h"', '#include "GeniI2c.h"')
    model = model.replace('Engine *e = ctx;', 'Engine *e = static_cast<Engine *>(ctx);')
    model = model.replace('int main(void)', 'int upstream_geni_tests(void)')
    methods = '\n'.join([method(source, 'readRegister', 'int'),
                         *(method(source, name) for name in ('kick', 'noteFailure', 'completeLevel', 'finished'))])
    methods = methods.replace('engineActive(i)', 'static_cast<::Engine *>(e.bus.io.ctx)->active')
    initial_guard = source.split('    void publishFirst() {', 1)[1].split('        published = true;', 1)[0]
    test = out / 'input-backlight-host.cpp'
    fail = '\nstatic void fail(int step, int error) {native_backlight_stats[0] = -step; native_backlight_stats[9] = error;}\n'
    body = HARNESS.replace('struct Driver {', fail + 'struct Driver {').replace('INITIAL_GUARD', initial_guard)
    test.write_text('#include <initializer_list>\n' + model + irq_harness() + body.replace('METHODS', methods))
    obj = out / 'GeniI2c.o'
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g', '-O1']
    subprocess.run(['clang', '-std=c11', *flags, '-c', str(ROOT / 'package/GeniI2c.c'), '-o', str(obj)], check=True)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                    *flags, '-I', str(ROOT / 'package'), str(test), str(obj), '-o', str(out / 'input-backlight-host')], check=True)
    result = subprocess.run([str(out / 'input-backlight-host')], text=True, capture_output=True)
    (out / 'host-test.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    print(result.stdout.strip())
    (out / 'host-test.json').write_text(json.dumps(dict(passed=True, sanitizers=['ASan', 'UBSan'],
        methods=['buttonsIrq', 'readRegister', 'kick', 'noteFailure', 'finished'], hardware_verified=False), indent=2) + '\n')


if __name__ == '__main__':
    main()
