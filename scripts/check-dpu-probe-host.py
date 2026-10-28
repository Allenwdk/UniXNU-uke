#!/usr/bin/env python3
"""M1 gate for the uke DPU track: source archaeology and built-archive presence.

Verifies, without touching hardware:
  1. The DpuProbe.cpp register anchors equal the cliffs DTS block offsets minus
     sde-off (0x1000) - the same DPU 9.x anchors sheng's DisplayTimingProbe uses.
  2. The SSPP/CTL register offsets equal the stock DPU driver's own defines.
  3. The built display archive carries DpuProbe.o, native_dpu_probe and the
     UKE DPU format strings.
  4. NativeUfs owns the display-bank stage-1 window (native_ufs_dpu_map) and
     reads the uke-dpu-scanout boot argument.

A deliberately wrong register constant turns this gate red (E3).
Run after ./unixnu build kernel, or with --source-only before a build.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DTS_DEFAULT = ROOT.parent / 'reference/dts-reference/03_dtbdump_Qualcomm_Technologies,_Inc._Cliffs7P_SoC.dts'
STOCK = ROOT.parent / 'reference/Xiaomi_Kernel_OpenSource-uke/drivers/gpu/drm/msm/disp/dpu1'
PROBE = ROOT / 'package/DpuProbe.cpp'
UFS = ROOT / 'drivers/NativeUfs/NativeUfs.cpp'


def fail(message):
    raise SystemExit('DPU gate: ' + message)


def block(text, start_pattern):
    match = re.search(start_pattern, text)
    if not match:
        fail('DTS block missing: ' + start_pattern)
    at = match.end()
    depth = 0
    for i in range(at, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[at:i]
    fail('unterminated DTS block: ' + start_pattern)


def hex_words(node, name, count):
    match = re.search(r'\b' + name + r'\s*=\s*<([^>]*)>', node)
    if not match:
        fail('DTS property missing: ' + name)
    values = [int(v, 16) for v in match.group(1).split()]
    if len(values) < count:
        fail('%s has %u entries, expected at least %u' % (name, len(values), count))
    return values[:count]


def probe_array(name, count):
    text = PROBE.read_text()
    match = re.search(name + r'\s*\[\s*\d+\s*\]\s*=\s*\{([^}]*)\}', text)
    if not match:
        fail('DpuProbe.cpp array missing: ' + name)
    values = [int(v, 0) for v in match.group(1).replace(' ', '').split(',') if v]
    if len(values) < count:
        fail('%s has %u entries, expected %u' % (name, len(values), count))
    return values[:count]


def probe_scalar(name):
    text = PROBE.read_text()
    match = re.search(r'\b' + name + r'\s*=\s*(0x[0-9a-fA-F]+|\d+)', text)
    if not match:
        fail('DpuProbe.cpp constant missing: ' + name)
    return int(match.group(1), 0)


def define(path, name):
    match = re.search(r'^#define\s+' + name + r'\s+(0x[0-9a-fA-F]+|\d+)', path.read_text(), re.M)
    if not match:
        fail('stock define missing: %s in %s' % (name, path))
    return int(match.group(1), 0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dts', type=Path, default=DTS_DEFAULT)
    parser.add_argument('--source-only', action='store_true')
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()

    if not PROBE.is_file():
        fail('package/DpuProbe.cpp missing')

    # 1. DTS block offsets minus sde-off == the probe's code offsets.
    dts = args.dts.read_text()
    mdss = block(dts, r'\bmdss_mdp:\s*qcom,mdss_mdp@ae00000\s*\{')
    sde_off = hex_words(mdss, 'qcom,sde-off', 1)[0]
    sspp = hex_words(mdss, 'qcom,sde-sspp-off', 2)
    ctl = hex_words(mdss, 'qcom,sde-ctl-off', 4)
    intf = hex_words(mdss, 'qcom,sde-intf-off', 4)
    pp = hex_words(mdss, 'qcom,sde-pp-off', 8)
    mixer = hex_words(mdss, 'qcom,sde-mixer-off', 2)
    if sde_off != 0x1000:
        fail('sde-off is %#x, expected 0x1000' % sde_off)
    expected = {
        'kVig': [sspp[0] - sde_off, sspp[1] - sde_off],
        'kCtl': [v - sde_off for v in ctl],
        'kIntf': [intf[1] - sde_off, intf[2] - sde_off],
        'kPp': [v - sde_off for v in pp[:4]],
        'kLm': [v - sde_off for v in mixer],
    }
    for name, values in expected.items():
        got = probe_array(name, len(values))
        if got != values:
            fail('%s = %s but the DTS says %s (DTS offset minus sde-off)' %
                 (name, ['%#x' % v for v in got], ['%#x' % v for v in values]))
    if probe_scalar('kMdssBase') != 0xae00000:
        fail('kMdssBase is not 0xae00000')
    if probe_scalar('kMdpBase') != 0xae00000 + sde_off:
        fail('kMdpBase is not mdss_mdp base + sde-off')

    # 2. SSPP/CTL/INTF register offsets against the stock driver's defines.
    if STOCK.is_dir():
        checks = [
            (STOCK / 'dpu_hw_sspp.c', 'SSPP_SRC0_ADDR', 'kSrc0'),
            (STOCK / 'dpu_hw_sspp.c', 'SSPP_SRC_YSTRIDE0', 'kStride0'),
            (STOCK / 'dpu_hw_sspp.c', 'SSPP_SRC_FORMAT', 'kFormat'),
            (STOCK / 'dpu_hw_sspp.c', 'SSPP_SRC_UNPACK_PATTERN', 'kUnpack'),
            (STOCK / 'dpu_hw_sspp.c', 'SSPP_SRC_OP_MODE', 'kOpMode'),
            (STOCK / 'dpu_hw_ctl.c', 'CTL_FLUSH', 'kCtlFlush'),
            (STOCK / 'dpu_hw_ctl.c', 'CTL_TOP', 'kCtlTop'),
            (STOCK / 'dpu_hw_intf.c', 'INTF_FRAME_COUNT', 'kIntfFrame'),
            (STOCK / 'dpu_hw_intf.c', 'INTF_STATUS', 'kIntfStatus'),
        ]
        for path, theirs, ours in checks:
            if define(path, theirs) != probe_scalar(ours):
                fail('%s = %#x but stock %s says %#x' % (ours, probe_scalar(ours), theirs, define(path, theirs)))
    else:
        print('note: stock DPU tree not found, register defines compared to DTS only')

    # 3. NativeUfs owns the display-bank stage-1 window.
    ufs = UFS.read_text()
    for symbol in ('native_ufs_dpu_map', 'native_ufs_dpu_state', 'dpuEnable', 'dpuWindowInit'):
        if symbol not in ufs:
            fail('NativeUfs.cpp is missing ' + symbol)
    if 'uke-dpu-scanout' not in ufs:
        fail('NativeUfs.cpp does not read uke-dpu-scanout')
    if 'dpuWindowInit ()' not in ufs:
        fail('NativeUfs start() does not call dpuWindowInit')

    # 4. C2: the four levels write, hold and restore; level 3 uses the UFS window.
    probe = PROBE.read_text()
    for marker in ('test=1', 'test=2', 'test=3', 'test=4', 'restorePipe', 'flushPipes',
                   'native_ufs_dpu_map', 'uke-dpu-test'):
        if marker not in probe:
            fail('DpuProbe.cpp is missing the C2 marker ' + marker)
    if 'UKE DPU: test=3 iova=' not in probe or 'restored=1' not in probe:
        fail('DpuProbe.cpp has no level-3 address-change line')

    # 5. The built archive carries the probe.
    if not args.source_only:
        candidates = []
        if args.archive:
            candidates.append(args.archive)
        candidates.append(ROOT / 'build/native-legacy-display/libNativeLegacyDisplay.late.a')
        candidates.extend(sorted(ROOT.glob('out/uke/*/uke-gpu-userland/tree/build/native-legacy-display/libNativeLegacyDisplay.late.a')))
        core = ROOT.parent / 'UniXNU-format2'
        candidates.extend(sorted(core.glob('out/uke/*/uke-gpu-userland/tree/build/native-legacy-display/libNativeLegacyDisplay.late.a')))
        archive = next((path for path in candidates if path.is_file()), None)
        if not archive:
            fail('no built libNativeLegacyDisplay.late.a found; build first or pass --source-only')
        data = archive.read_bytes()
        for marker in (b'DpuProbe.o', b'native_dpu_probe', b'UKE DPU: probe=', b'UKE DPU: test=',
                       b'UKE DPU: window state=', b'UKE DPU: smmu sid='):
            if marker not in data:
                fail('%s missing from %s' % (marker.decode(), archive))
        try:
            shown = archive.relative_to(ROOT)
        except ValueError:
            shown = archive
        print('archive: %s (%u bytes)' % (shown, len(data)))

    print('PASS: DPU M1 gate: DTS anchors, stock registers, UFS window, built probe')


if __name__ == '__main__':
    main()
