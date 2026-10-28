#!/usr/bin/env python3
"""Summarize the UKE DPU log lines of one boot and check the M1/M2 criteria.

Usage: parse-dpu-log.py <boot-log>            (text log, dmesg, or a serial capture)
       parse-dpu-log.py -                     (stdin)
The input is whatever text contains the kernel log; every line with "UKE DPU:" is
used and the rest is ignored, so a full boot log or a pasted excerpt both work.
"""
import re
import sys
from pathlib import Path


def words(line, name):
    match = re.search(name + r'=([0-9a-fA-F]+)', line)
    return int(match.group(1), 16) if match else None


def size_decode(value):
    if value is None:
        return None
    return value & 0xffff, value >> 16


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    text = sys.stdin.read() if sys.argv[1] == '-' else Path(sys.argv[1]).read_text(errors='replace')
    lines = [line for line in text.splitlines() if 'UKE DPU:' in line]
    if not lines:
        raise SystemExit('no UKE DPU lines found')

    gop = None
    probe = done = False
    live_ok = False
    smmu_ok = None
    tests = []
    vig = {}
    print('UKE DPU summary (%u lines)' % len(lines))
    for line in lines:
        body = line.split('UKE DPU:', 1)[1].strip()
        if body.startswith('probe='):
            probe = words(body, 'mdp') not in (None, 0, 0xffffffff)
            print('  probe: ' + body)
        elif body.startswith('gop '):
            gop = words(body, 'base')
            print('  gop:   ' + body)
        elif body.startswith(('vig0 ', 'vig1 ')):
            name = body.split()[0]
            size = words(body, 'size')
            src = words(body, 'src')
            vig[name] = (size_decode(size), src)
            print('  %s: size=%s src=%s' % (name, size_decode(size), hex(src) if src is not None else None))
        elif body.startswith('live '):
            deltas = re.findall(r'frame=(\d+)->(\d+)/(\d+)->(\d+)', body)
            if deltas:
                d = []
                for row in deltas:
                    d.extend([int(row[1]) - int(row[0]), int(row[3]) - int(row[2])])
                live_ok = any(x > 0 for x in d)
                print('  live: frame deltas %s' % d)
            else:
                print('  live: ' + body)
        elif body.startswith('window '):
            print('  window: ' + body)
        elif body.startswith('smmu sid='):
            sid = words(body, 'sid')
            cbar = words(body, 'cbar') or 0
            sctlr = words(body, 'sctlr') or 0
            if sid == 0x1c00:
                smmu_ok = ((cbar >> 16) & 3) == 1 and (sctlr & 1) == 0
            print('  smmu %04x: cbar_type=%u sctlr_bits=%08x %s' %
                  (sid or 0, (cbar >> 16) & 3, sctlr, 'ok' if smmu_ok else 'check'))
        elif body.startswith('probe done'):
            done = True
            print('  done:  ' + body)
        elif body.startswith('test='):
            tests.append(body)
            print('  test:  ' + body)

    print()
    print('M1: probe=%s gop=%s live=%s done=%s smmu_window=%s' %
          (probe, hex(gop) if gop else None, live_ok, done, smmu_ok))
    if gop and vig:
        for name, (size, src) in vig.items():
            if size and src is not None:
                print('  %s source %s vs gop %s: %s' % (
                    name, hex(src), hex(gop), 'MATCH' if src == gop else 'differs'))
    if tests:
        print('M2 rows:')
        for row in tests:
            print('  ' + row)
    verdict = probe and done and live_ok
    print('\nM1 VERDICT:', 'PASS (read-only snapshot complete)' if verdict else 'INCOMPLETE')
    if any('test=3 iova=' in t for t in tests):
        print('M2 address test: row present; check the screen for the gradient and restored=1')


if __name__ == '__main__':
    main()
