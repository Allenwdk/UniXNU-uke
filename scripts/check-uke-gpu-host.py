#!/usr/bin/env python3
"""Host check for the A732 GPU profile and generated headers (no macOS, no device).

Runs on the Linux validation runner:
  1. compiles the immutable profile test from the checked-out NativeAdreno;
  2. runs prepare-uke-gmu.py in build mode (weak firmware) to assemble the
     three headers from the recorded live-DT facts;
  3. checks the exact chip, GMU, OPP and command-DB values in those headers.

It never reads MMIO and never claims a hardware result.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def find_kit(explicit):
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    candidates += [ROOT / 'drivers/NativeAdreno', ROOT.parent / 'UniXNU/drivers/NativeAdreno',
                   ROOT.parent.parent / 'UniXNU/drivers/NativeAdreno']
    for candidate in candidates:
        if (candidate / 'NativeAdrenoProfile.cpp').is_file():
            return candidate.resolve()
    raise SystemExit('NativeAdreno sources not found; pass --kit DIR')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kit', type=Path)
    args = parser.parse_args()
    kit = find_kit(args.kit)
    out = ROOT / 'build/uke-gpu-host'
    out.mkdir(parents=True, exist_ok=True)
    test = out / 'test-profile'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(kit), str(kit / 'NativeAdrenoProfile.cpp'),
                    str(kit / 'tests/test_profile.cpp'), '-o', str(test)],
                   check=True, timeout=120)
    result = subprocess.run([str(test)], check=True, capture_output=True, text=True, timeout=60)
    print(result.stdout, end='')
    if 'A740 and A732 exact profiles' not in result.stdout:
        raise SystemExit('profile test did not cover both chips')

    script = ROOT / 'scripts/prepare-uke-gmu.py'
    generated = subprocess.run([sys.executable, str(script), '--force'],
                               cwd=ROOT, check=True, capture_output=True, text=True,
                               timeout=60)
    print(generated.stdout, end='')
    base = ROOT / 'build/sheng-gmu'
    expected = {
        'ShengGmuSource.h': [
            r'#define SHENG_GMU_CHIP_ID UINT32_C\(0x43030b00\)',
            r'#define SHENG_GMU_FW_CHIP_ID UINT32_C\(0x70b0000\)',
            r'#define SHENG_GMU_CORE_GPU_DELTA 0x68000u',
            r'#define SHENG_GMU_FIRMWARE_BYTES 80680u',
            r'#define SHENG_GMU_RSC_SETUP_COUNT 19u',
        ],
        'ShengGmuVotesSource.h': [
            r'#define SHENG_GMU_VOTES_CHIP_ID 0x43030b00u',
            r'#define SHENG_GPU_OPPS 10u',
            r'#define SHENG_GMU_OPPS 3u',
            r'#define SHENG_GPU_BANDWIDTHS 7u',
        ],
        'ShengGmuCommandDbSource.h': [
            r'#define SHENG_GMU_COMMAND_DB_BASE UINT64_C\(0x81c60000\)',
            r'#define SHENG_GMU_COMMAND_DB_BYTES 131072u',
        ],
    }
    for name, patterns in expected.items():
        path = base / name
        if not path.is_file():
            raise SystemExit(path.name + ' was not generated')
        text = path.read_text()
        for pattern in patterns:
            if not re.search(pattern, text):
                raise SystemExit('%s lacks %s' % (name, pattern))
    print('PASS: A732 profile host test and the three generated headers carry the recorded '
          'chip, GMU, OPP and command-DB values')


if __name__ == '__main__':
    main()
