#!/usr/bin/env python3
"""Check a decoded uke boot log for the GPU addon milestones.

The addon images report through the boot page; addon4 mirrors the rows into
the kernel log, where the userdata window keeps them. Pass the decoded log
(tools/decode-ukelog.py) and this script reads the last boot's rows:

  UKE GPU addon: stage=3 files=7 bytes=39398839 error=0
  UKE GPU3: M a5.0/5.0 k0 s5/0 r1 o1/0/0 n12 d10:481/120
  UKE GPU4: K12 6800 6801 6802 6810 6803
  UKE GPU7: R ...
  UKE GPU8: T ...

The heartbeat's slot 6 is printed as UKE GPU3 and slot 7 as UKE GPU4
(setRow prints slot-3). The window may mix boots; the last occurrence of each
row is used.

  check-gpu-addon-boot.py boot.log [--mode auto|probe|stable]

--mode probe expects no device markers (the blob omits .publish, so the plugin
runs its user test and returns nil); auto infers the mode from the markers.
Exit status is 0 when every required check passes, 1 otherwise.
"""
import argparse
import re
import sys

ADDON = re.compile(r'UKE GPU addon: stage=(-?\d+)(?: files=(\d+) bytes=(\d+) error=(-?\d+))?')
MROW = re.compile(
    r'UKE GPU3: M a(-?\d+)\.([0-9a-f]+)/(-?\d+)\.([0-9a-f]+) k([0-9a-f]+) '
    r's(\d+)/([0-9a-f]+) r(\d+) o(\d+)/(\d+)/(\d+) n(\d+) d(\d)(\d):(\d+)/(\d+)')
KROW = re.compile(r'UKE GPU4: K(\d+)((?: [0-9a-f]+)*)')
REPORT = re.compile(r'UKE GPU7: R (\S.*)')
PHASE = re.compile(r'UKE GPU8: (\S.*)')

SUCCESS_MARKS = {0x6802, 0x6803} | set(range(0x6810, 0x6814))
FAIL_MARKS = {0x68f1: 'no Turnip beside the plugin',
              0x68f2: 'no device: every cache directory and open failed',
              0x68f3: 'the scanout client threw'}


def last(pattern, lines):
    found = None
    for line in lines:
        match = pattern.search(line)
        if match:
            found = match
    return found


def analyze(text, mode):
    lines = text.splitlines()
    failures, warnings = [], []
    out = []

    addon = last(ADDON, lines)
    if not addon:
        failures.append('no "UKE GPU addon:" line: sheng-gpu-addon=1 missing or the install did not run')
    else:
        stage = int(addon.group(1))
        error = int(addon.group(4) or -1)
        if stage == -1:
            failures.append('addon install failed (stage=-1 error=%d)' % error)
        elif stage == 2:
            warnings.append('addon install was still writing when the log was taken (stage=2)')
        elif stage not in (3, 4):
            failures.append('unexpected addon install stage %d (error=%d)' % (stage, error))
        out.append('addon install: stage=%d files=%s bytes=%s error=%s' % (
            stage, addon.group(2), addon.group(3), addon.group(4)))

    row = last(MROW, lines)
    opens = None
    if not row:
        failures.append('no heartbeat row (UKE GPU3: M ...): the addon mount never reported')
    else:
        ext, daemon_view = int(row.group(1)), int(row.group(3))
        opens, closes = int(row.group(9)), int(row.group(11))
        seen, alive = int(row.group(13)), int(row.group(14))
        if ext != 5:
            failures.append('Extensions view is not complete (a%d): bundle/Info.plist not visible' % ext)
        if daemon_view != 5:
            failures.append('LaunchDaemons view is not complete (%d): daemon plist not visible' % daemon_view)
        if opens == 0:
            failures.append('no AGX user client opened (o0): check the sandbox grant and the plugin markers')
        out.append('heartbeat: a%d/%d opens=%d closes=%d name reads=%s daemon seen=%d alive=%d pid=%s' % (
            ext, daemon_view, opens, closes, row.group(12), seen, alive, row.group(15)))
        if not seen:
            warnings.append('the adrenoprobed daemon was never seen (d0): check launchd and the daemons view')
        if alive:
            warnings.append('the adrenoprobed daemon is still alive: it exits after one test')

    marks = []
    krow = last(KROW, lines)
    if not krow:
        warnings.append('no plugin marker row (UKE GPU4: K ...): the plugin did not run yet')
    else:
        marks = [int(token, 16) for token in krow.group(2).split()]
        total = int(krow.group(1))
        out.append('plugin markers: total=%d shown=%s' % (total, ' '.join('%x' % m for m in marks)))
    if mode == 'auto':
        mode = 'stable' if any(m in SUCCESS_MARKS for m in marks) else 'probe'
        out.append('mode: %s (inferred from the markers)' % mode)
    for mark, why in FAIL_MARKS.items():
        if mark in marks:
            failures.append('plugin marker %x: %s' % (mark, why))
    if mode == 'stable':
        if not any(m in SUCCESS_MARKS for m in marks):
            warnings.append('no device-created marker (6802/6803/681x) in the last seven; '
                            'markers may have rolled over, opens is the primary evidence')
    elif mode == 'probe':
        if any(m in SUCCESS_MARKS for m in marks):
            warnings.append('a device-created marker is present in a probe image')
        if not krow or int(krow.group(1)) == 0:
            failures.append('the probe user test left no markers (K0)')

    report = last(REPORT, lines)
    phase = last(PHASE, lines)
    if report:
        out.append('report: %s' % report.group(1)[:100])
    else:
        warnings.append('no R report row (UKE GPU7): the user test has not reported')
    if phase:
        out.append('phase: %s' % phase.group(1)[:100])

    for line in out:
        print('INFO: ' + line)
    for line in warnings:
        print('WARN: ' + line)
    for line in failures:
        print('FAIL: ' + line)
    if failures:
        return 1
    print('PASS: the addon %s boot looks complete' % mode)
    return 0


SELF_STABLE = '''\
UKE GPU addon: stage=3 files=7 bytes=39398862 error=0
UKE GPU3: M a5.0/5.0 k0 s5/0 r1 o2/0/1 n9 d10:481/120
UKE GPU4: K9 6800 6801 6802 6810 6803
UKE GPU7: R 512/512 ok
UKE GPU8: T 12
'''
SELF_PROBE = '''\
UKE GPU addon: stage=4 files=7 bytes=39398862 error=0
UKE GPU3: M a5.0/5.0 k0 s5/0 r1 o3/0/0 n9 d10:481/120
UKE GPU4: K6 3001 3009 4002 5001
'''
SELF_FAIL = '''\
UKE GPU addon: stage=-1 files=0 bytes=0 error=2
UKE GPU3: M a4.2/3.0 k0 s5/0 r1 o0/0/0 n0 d00:0/80
UKE GPU4: K3 68f2
'''


def self_test():
    cases = [(SELF_STABLE, 'auto', 0), (SELF_PROBE, 'auto', 0),
             (SELF_STABLE, 'stable', 0), (SELF_PROBE, 'probe', 0),
             (SELF_FAIL, 'auto', 1)]
    for text, mode, expected in cases:
        if analyze(text, mode) != expected:
            print('FAIL: self-test case mode=%s expected %d' % (mode, expected))
            return 1
    print('PASS: self-test')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log', nargs='?', help='decoded kernel log')
    parser.add_argument('--mode', choices=('auto', 'probe', 'stable'), default='auto')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.log:
        parser.error('a decoded log or --self-test is required')
    with open(args.log, encoding='utf-8', errors='replace') as file:
        return analyze(file.read(), args.mode)


if __name__ == '__main__':
    sys.exit(main())
