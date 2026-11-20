#!/usr/bin/env python3
"""Exercise the Uke clock recipe and production rail-release policy on the host."""
# SPDX-License-Identifier: BSD-2-Clause-Patent
from pathlib import Path
import subprocess
import tempfile

PACKAGE = Path(__file__).resolve().parents[1] / 'package'


def main():
    board = (PACKAGE / 'UkeUsbBoard.cpp').read_text()
    rail_policy = board[board.index('struct WcnRail'):board.index('} // namespace', board.index('struct WcnRail'))]
    bluetooth = (PACKAGE / 'BluetoothProbe.cpp').read_text()
    pin_policy = bluetooth[bluetooth.index('constexpr uint32_t kBtEnableCfg'):
                           bluetooth.index('constexpr uint32_t kDmaBytes')]
    with tempfile.TemporaryDirectory(prefix='uke-bluetooth-platform-') as temp:
        work = Path(temp)
        source = work / 'test.cpp'
        source.write_text(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "UkeBluetoothClock.h"
#include "WcnPower.h"

struct QcomRpmhResource {uint32_t address;};
uint32_t gRadioOwners, gBluetoothVoted;
int qcom_cmd_db_find(const uint8_t *, size_t, const char *, QcomRpmhResource *resource) {
    resource->address = 0x4200;
    return 0;
}
''' + rail_policy + pin_policy + r'''
struct Clock {
    uint32_t regs[6]{};
    bool stalled{};
    static uint32_t read(void *context, uint32_t offset) {
        return static_cast<Clock *>(context)->regs[(offset - 0x1e280) / 4];
    }
    static void write(void *context, uint32_t offset, uint32_t value) {
        auto &clock = *static_cast<Clock *>(context);
        const auto index = (offset - 0x1e280) / 4;
        clock.regs[index] = index || clock.stalled ? value : value & ~1u;
    }
    static void delay(void *, unsigned) {}
};

int main() {
    assert(btEnableControl(0x00000a01u) == 0x00001a01u);
    assert(btEnableControl(0x00001a01u) == 0x00001a01u);
    assert(btEnableControl(0x800008fcu) == 0x80001a01u);
    assert(btEnableControl(0x400000fcu) == 0x40000201u);
    assert(rxWakeControl(0) == 0x31au);
    assert((rxWakeControl(0x79u) & kRxWakeFields) == 0x31au);
    assert(rxWakeControl(0x80000441u) == 0x8000035au);
    assert((rxWakeControl(0x000007ffu) | kRxWakeEnable) == 0x3fbu);
    std::vector<const char *> disabled;
    bool fail = false;
    auto submit = [&](const char *name, const uint32_t *address, const uint32_t *value, unsigned count) {
        assert(count == 1 && *address == 0x4204 && *value == 0);
        if (fail) return false;
        disabled.push_back(name);
        return true;
    };
    gRadioOwners = 1u << UKE_WCN_WLAN;
    gBluetoothVoted = 0x1f;
    assert(releaseBluetoothRails(nullptr, 0, submit));
    assert(disabled.size() == 1 && !strcmp(disabled[0], "smpe3") && !gBluetoothVoted);
    disabled.clear();
    gRadioOwners = 0;
    gBluetoothVoted = 0x1f;
    fail = true;
    assert(!releaseBluetoothRails(nullptr, 0, submit) && gBluetoothVoted == 0x1f);
    fail = false;
    assert(releaseBluetoothRails(nullptr, 0, submit) && !gBluetoothVoted);
    assert(disabled.size() == 3 && !strcmp(disabled[0], "smpe3") &&
           !strcmp(disabled[1], "smpb1") && !strcmp(disabled[2], "smpb3"));

    Clock clock;
    clock.regs[5] = 1;
    const uke_bluetooth_clock_io io{&clock, Clock::read, Clock::write, Clock::delay};
    uint32_t rate = 0;
    assert(!uke_bluetooth_clock_source(&io, 115200, &rate) && rate == 7372800 && clock.regs[2] == 48);
    assert((clock.regs[1] & 0x700) == 0 && clock.regs[3] == (~77u & 0xffffu) &&
           clock.regs[4] == (~125u & 0xffffu));
    assert(uint64_t{19200000} * clock.regs[2] / 125 == rate);
    assert(!uke_bluetooth_clock_source(&io, 3000000, &rate) && rate == 48000000 && clock.regs[2] == 4);
    assert((clock.regs[1] & 0x700) == 0x600 && clock.regs[3] == (~21u & 0xffffu) && clock.regs[4] == (~25u & 0xffffu) && !clock.regs[5]);
    assert(uke_bluetooth_clock_source(&io, 999, &rate) < 0);
    clock.stalled = true;
    assert(uke_bluetooth_clock_source(&io, 3000000, &rate) < 0);
}
''')
        flags = ['-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I' + str(PACKAGE)]
        subprocess.run(['clang', '-std=c11', *flags, '-c', PACKAGE / 'UkeBluetoothClock.c',
                        '-o', work / 'clock.o'], check=True)
        subprocess.run(['clang++', '-std=c++17', *flags, source, work / 'clock.o',
                        '-o', work / 'platform'], check=True)
        subprocess.run([work / 'platform'], check=True)
    print('PASS: Uke RX wake routing, BT_EN ownership, QUP clock rates and timeout, rail off/retry and shared platform votes')


if __name__ == '__main__':
    main()
