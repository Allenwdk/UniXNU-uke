#!/usr/bin/env python3
"""Host check for the board's firmware-name dispatch (WifiAth11k.cpp fwGet).

The stock partition holds two WiFi assets and nothing else. Before 2026-11-08
every name except regdb was answered with the board-data ELF, so ath11k's
board-2.bin container parser read an ELF ("found invalid board magic") and
firmware-2.bin/wlan_mac.bin would have received board data too. This check
slices the production dispatch out of the board source and drives it with the
exact names the D61F boot requested, so a refactor that widens or narrows the
whitelist fails here instead of on the device.

Run: python3 scripts/check-uke-wifi-assets.py [--report PATH]
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'package/WifiAth11k.cpp'
BEGIN = 'enum { kStockNone'
END = 'const struct lx_platform gPlatform'

HARNESS = r'''
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cerrno>

// include/nw_wcn6750_qmi.h in the driver tree; the board picks its stock asset
// by type, so the test holds the same two values the device uses.
enum { NW_WCN6750_BDF_ELF = 1, NW_WCN6750_REGDB = 4 };

// The board's stock reader: one asset per type, or a forced failure.
static int gFailTypes;
static int gLoads;
static uint8_t gRegdb[16], gBdf[16];
int uke_wlfw_load_stock_data(unsigned type, uint8_t **data, uint32_t *length) {
    gLoads++;
    if (gFailTypes & (int)type) return EIO;
    if (type == 4) { *data = gRegdb; *length = sizeof gRegdb; return 0; }
    if (type == 1) { *data = gBdf; *length = sizeof gBdf; return 0; }
    return EINVAL;
}

// ---- production dispatch, sliced verbatim from package/WifiAth11k.cpp ----
// __SLICED__
// ---- end of sliced production code ----

static int gFails;
static void expect(const char *name, int want, const uint8_t *wantData, int wantLoads) {
    const uint8_t *data = nullptr; size_t size = 0;
    gLoads = 0;
    const int got = fwGet(name, &data, &size);
    const int ok = got == want && gLoads == wantLoads && (want != 0 || data == wantData);
    if (!ok) { gFails++; std::printf("FAIL %s: got %d loads %d data %p, want %d loads %d data %p\n",
                                    name, got, gLoads, (const void *)data, want, wantLoads, (const void *)wantData); }
}

int main(void) {
    // A transient stock-read failure must not poison the slot.
    gFailTypes = 1;
    expect("board.bin", -ENOENT, nullptr, 1);
    gFailTypes = 0;

    // The two assets the device really holds, requested by full path as ath11k does.
    expect("ath11k/WCN6750/hw1.0/board.bin", 0, gBdf, 1);
    expect("ath11k/WCN6750/hw1.0/board.bin", 0, gBdf, 0);   // cached after the first read
    expect("ath11k/WCN6750/hw1.0/regdb.bin", 0, gRegdb, 1);
    expect("regdb_xiaomi.bin", 0, gRegdb, 0);

    // Names the device has no file for: the container format must not receive the ELF.
    expect("ath11k/WCN6750/hw1.0/board-2.bin", -ENOENT, nullptr, 0);
    expect("ath11k/WCN6750/hw1.0/board-2.bin", -ENOENT, nullptr, 0);
    expect("firmware-2.bin", -ENOENT, nullptr, 0);
    expect("amss.bin", -ENOENT, nullptr, 0);
    expect("m3.bin", -ENOENT, nullptr, 0);
    expect("wlan_mac.bin", -ENOENT, nullptr, 0);
    expect("bdwlan.bin", -ENOENT, nullptr, 0);

    if (gFails) { std::printf("%d asset dispatch checks failed\n", gFails); return 1; }
    std::printf("PASS: board.bin and regdb.bin dispatch to the verified stock assets, board-2.bin "
                "and every absent name report ENOENT without touching the stock reader, and a failed "
                "read leaves the slot empty\n");
    return 0;
}
'''


def slice_source() -> str:
    text = SOURCE.read_text()
    start = text.index(BEGIN)
    end = text.index(END, start)
    return text[start:end]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--report')
    args = parser.parse_args()
    sliced = slice_source()
    with tempfile.TemporaryDirectory() as tmp:
        harness = Path(tmp) / 'assets.cpp'
        binary = Path(tmp) / 'assets'
        harness.write_text(HARNESS.replace('// __SLICED__', sliced))
        build = subprocess.run(['clang++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', str(harness), '-o', str(binary)],
                               text=True, capture_output=True)
        if build.returncode:
            print(build.stderr)
            return 1
        run = subprocess.run([str(binary)], text=True, capture_output=True)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        report = {
            'passed': run.returncode == 0,
            'source': str(SOURCE.relative_to(ROOT)),
            'source_sha256': __import__('hashlib').sha256(SOURCE.read_bytes()).hexdigest(),
            'sliced_lines': sliced.count('\n'),
            'stock_reader_substituted': True,
            'hardware_verified': False,
        }
        if args.report:
            Path(args.report).write_text(json.dumps(report, indent=1) + '\n')
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
