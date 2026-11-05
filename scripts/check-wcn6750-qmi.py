#!/usr/bin/env python3
"""Focused peer fixture for the production read-only WLFW identity observer."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--protocol-dir', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    module = args.module.resolve()
    subprocess.run(['python3', str(module / 'tools/stage-wcn6750-qmi.py')], check=True)
    with tempfile.TemporaryDirectory(prefix='uke-qmi-') as directory:
        directory = Path(directory)
        (directory / 'sys').mkdir()
        (directory / 'sys/systm.h').write_text('#include <stdio.h>\n#include <string.h>\n#define bzero(p,n) memset(p,0,n)\n')
        sources = [ROOT / 'package/WifiQmiHostTests.c', ROOT / 'package/WifiQrtr.c',
                   args.protocol_dir / 'QcomQrtr.c', module / 'out/wcn6750-qmi/Wcn6750Qmi.c',
                   module / 'deps/linux/drivers/soc/qcom/qmi_encdec.c']
        command = ['clang', '-std=gnu11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g',
                   '-fsanitize=address,undefined', '-fno-sanitize=alignment', '-Wno-incompatible-pointer-types',
                   '-include', str(module / 'shim/libc/errno.h'), '-include', 'lx_pci.h']
        for include in [directory, ROOT / 'package', args.protocol_dir, module / 'include',
                        module / 'shim/include', module / 'out/pinned', module / 'out/ath11k-src']:
            command += ['-I', str(include)]
        executable = directory / 'test'
        subprocess.run([*command, *map(str, sources), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
        print(result.stdout, end=''); print(result.stderr, end='')
        if result.returncode:
            raise SystemExit(result.returncode)
    args.report.write_text(json.dumps({'passed': True, 'hardware_verified': False,
        'sanitizers': ['address', 'undefined'], 'alignment_sanitizer_disabled': True,
        'alignment_reason': 'Original Linux codec uses unaligned wire-field casts on normal packet buffers.',
        'output': result.stdout}, indent=2) + '\n')


if __name__ == '__main__':
    main()
