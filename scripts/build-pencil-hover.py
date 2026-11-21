#!/usr/bin/env python3
"""Build the signed 24A446 Uke stylus decoder and native HID hover service."""
# SPDX-License-Identifier: BSD-2-Clause-Patent
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import runpy
import shutil
import subprocess

DEVICE = Path(__file__).resolve().parents[1]
LABEL = 'org.unixnu.uke.pen-hover'
BINARY = 'bluetooth-bin/uke-pen-hover'
PROFILE = 'pencil-pairing/calibration.ini'
JOB = f'/System/Library/LaunchDaemons/{LABEL}.plist'
ENTITLEMENTS = {
    'com.apple.private.hid.client.event-dispatch': True,
    'com.apple.hid.manager.user-access-protected': True,
    'com.apple.security.iokit-user-client-class': ['IOHIDLibUserClient'],
}

PIANO_INPUTS = {
    'piano-pen-core.cpp': '186554fe143f95836f18d542ae8ef5de1590162ef333c179884d6fef43326a63',
    'piano-pen-core.h': 'c4cda024e69ea6f8ca1f208cddb0e74d23b31a8ecf0598d424e6168f7d92ec81',
    'piano-pen-decoder.cpp': '5f639604cc57255275158b40a3e16104572fdd7034c2fa6316126d089d7a46a8',
    'piano-pen-decoder.hpp': '3b469a538d1b3de8f32cb19b529ccf40d12141d2e6fb9525cf91aa1f32e21369',
    'piano-pen-frame.c': '4a9723da67550859dcdfb29dae64776a7ac5c785327458dc8b986bfe27f68d1f',
    'piano-pen-frame.h': '7691e2a21be9bc1e0db3a329fdbad73736c9e75030d525953784b31b69a9506d',
}
OEM_CALIBRATIONS = {
    'CSOT': 'baab492bf945ba1e769d5f84130c7879751a6028791b60187607e5f8b8091f21',
    'Tianma': 'fd1571567217dcfa57c4ff49b32709464ab09c071bcc7fa750cb6a866b276ed0',
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def prepare_decoder(piano, out):
    out.mkdir(parents=True, exist_ok=True)
    for name, digest in PIANO_INPUTS.items():
        data = (piano / name).read_bytes()
        if sha(data) != digest:
            raise ValueError(f'Piano input differs from the pinned source: {name}')
        (out / name).write_bytes(data)
    core = out / 'piano-pen-core.cpp'
    content = core.read_text()
    content = content.replace('(vendor != 1 && vendor != 3)', 'vendor != 3')
    content = content.replace('!= 100 ||', '!= 10 ||')
    content = content.replace('213599, 319999, vendor == 1 ? 8191 : 16383', '21359, 31999, 16383')
    begin = content.index('    // Vendor IDs select calibration slots')
    end = content.index('    const auto profile_value', begin)
    selection = '    // Uke stock libtouchreport selects stylus_2 for xiaomi_p81c.\n'
    content = content[:begin] + selection + '    const std::string prefix = "stylus_2";\n' + content[end:]
    # Drop incomplete hand quarters across frame gaps while retaining actual
    # pressure history and stylus state. Never replay multiple older pressures.
    reset = 'extern "C" void piano_pen_core_reset(piano_pen_core *core) {'
    assert content.count(reset) == 1
    content = content.replace(reset, '''extern "C" void uke_pen_core_drop_mutual_parts(piano_pen_core *core) {
    core->frame.packet_sum = core->frame.part_mask = 0;
    core->frame.frame[0x165] = 0;
}
''' + reset)
    pressure = 'if (!core->pressure.empty() && core->pressure.front().time <= time) {'
    assert content.count(pressure) == 1
    content = content.replace('// Consume one actual report per raw frame, as the OEM/peer bounded queue.',
                              '// Select the newest real pressure at or before this frame.')
    content = content.replace(pressure, 'while' + pressure[2:])
    position_reset = '''extern "C" void uke_pen_core_reset_position(piano_pen_core *core) {
    core->decoder.reset();
    core->have_frame_time = false;
    uke_pen_core_drop_mutual_parts(core);
}
'''
    assert content.count(reset) == 1
    core.write_text(content.replace(reset, position_reset + reset))
    header = out / 'piano-pen-core.h'
    declaration = 'void piano_pen_core_reset(struct piano_pen_core *core);'
    header.write_text(header.read_text().replace(declaration, declaration +
                      '\nvoid uke_pen_core_drop_mutual_parts(struct piano_pen_core *core);'
                      '\nvoid uke_pen_core_reset_position(struct piano_pen_core *core);'))
    frame = out / 'piano-pen-frame.c'
    content = frame.read_text()
    begin = content.index('    if (required>bytes)')
    end = content.index('    /* MiCode17/29 trailer', begin)
    content = content[:begin] + '''    const int compact=bytes == hand_start;
    if (!compact && bytes < required)
        return reject(error,"pen sample truncated before its trailer or inside a hand quarter");
    unsigned part=metadata[11];
    if (part<1 || part>4 || le16(metadata+12)!=quarter_bytes)
        return reject(error,"hand quarter number or length does not match v2 copy size");
    // The solver consumes the four pen matrices. Calibrated Uke captures
    // can omit the hand quarter, or carry a stale hand quarter from a write
    // by the controller. Check the full envelope and validate the pen
    // payload independently. Publish hand data only with the outer checksum.
    const uint32_t words=le32(packet+8);
    const uint16_t inverted=(uint16_t)(le16(packet+4)^0xffffu);
    if (!compact && (words>(PIANO_PEN_MAX_PACKET-20)/4 || 20+(size_t)words*4<required ||
        le32(packet+16)!=~words || le16(packet+12)!=inverted))
        return reject(error,"invalid original outer frame envelope");
    size_t outer_end=0, pen_end;
    const int hand_valid=!compact && checksum(packet,bytes,0,required,bytes,&outer_end);
''' + content[end:]
    accumulation = '    if (part==4) {'
    assert content.count(accumulation) == 1
    copy = '    for (size_t i=0;i<quarter;i++)\n        output->hand_parts[offset+i]=(int16_t)le16(packet+hand_start+i*2);\n    output->packet_sum+=part;output->part_mask|=1u<<(part-1);'
    assert content.count(copy) == 1
    content = content.replace(copy + '\n' + accumulation, '''    if (hand_valid) {
        for (size_t i=0;i<quarter;i++)
            output->hand_parts[offset+i]=(int16_t)le16(packet+hand_start+i*2);
        output->packet_sum+=part;output->part_mask|=1u<<(part-1);
    }
    if (!hand_valid) {
        output->packet_sum=0;output->part_mask=0;
    } else if (part==4) {''')
    frame.write_text(content)
    frame_header = out / 'piano-pen-frame.h'
    original_contract = ''' * removed. Both real additive checksums/complements are required; Android mmap
 * CRC-rewritten input and type17 are deliberately not normalized or bypassed.'''
    contract = ''' * removed. The original pen checksum is required. Hand samples additionally
 * require the original outer checksum. Android mmap CRC and type17 are rejected.'''
    content = frame_header.read_text()
    assert content.count(original_contract) == 1
    frame_header.write_text(content.replace(original_contract, contract))
    for name in ('Apache-2.0.txt', 'OPEN_CORE_ORIGIN.txt'):
        shutil.copyfile(piano / name, out / name)


def compact_calibration(raw, panel):
    if sha(raw) != OEM_CALIBRATIONS[panel]:
        raise ValueError(f'{panel} calibration differs from the same-board ODM input')
    sections = {}
    # Uke INIs use bare section names and consecutive lines for the mapping arrays.
    section = None
    for record in re.finditer(r'^[ \t]*([a-z_]+)[ \t]*\r?$|^[ \t]*(\w+)[ \t]*=[ \t]*({[^}]+}|-?\d+)',
                              raw.decode(), re.M):
        if record[1] is not None:
            section = sections.setdefault(record[1], {})
        elif section is not None:
            section[record[2]] = record[3]
    names = ('hal_row_num', 'hal_col_num', 'hal_node_num', 'display_x_resolution', 'display_y_resolution',
             'x_flip', 'y_flip', 'xy_flip')
    keys = ('en', 'num_col1', 'num_row1', 'num_col2', 'num_row2', 'angle', 'tilt_calibration_en',
            'tilt_calibration_thd', 'tilt_calibration_rate', 'tip_slope_tx', 'tip_slope_rx',
            'rx_l_edge_param', 'rx_r_edge_param', 'tx_l_edge_param', 'tx_r_edge_param',
            'limit_rx_start', 'limit_rx_end', 'limit_tx_start', 'limit_tx_end', 'up_threshold',
            'ring_threshold', 'kalman_q_div_min', 'kalman_q_div_max', 'kalman_lamda_div_min',
            'kalman_lamda_div_max', 'first_jitter_lock_dis', 'stable_jitter_lock_dis', 'move_jitter_lock_dis',
            '2_coor_diff', '2_tip_slope_tx', '2_tip_slope_rx', '2_tilt_calibration_thd', '2_tilt_calibration_rate')
    x = [int(number) for number in re.findall(r'-?\d+', sections['mapping']['default_mapping_x'])]
    y = [int(number) for number in re.findall(r'-?\d+', sections['mapping']['default_mapping_y'])]
    if len(x) != 2400 or len(y) != 2400 or x != [row for row in x[::60] for _ in range(60)] or y != y[:60] * 40:
        raise ValueError('expected separable Uke CSOT/Tianma calibration')
    stylus = ['stylus_' + key for key in keys]
    parts = {
        'project_infor': {'super_resolution': sections['project_infor']['super_resolution']},
        'hw': {key: sections['hw'][key] for key in names},
        'stylus': {key: sections['stylus'][key] for key in stylus},
        'calibration': {'format_version': '1'},
        'mapping': {'mapping_40': '{' + ','.join(map(str, x[::60])) + '}',
                    'mapping_60': '{' + ','.join(map(str, y[:60])) + '}'},
    }
    return ''.join(name + '\n' + ''.join(f'{key}={value}\n' for key, value in values.items())
                   for name, values in parts.items()).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('addon', 'trust', 'stock-cache-signature', 'output', 'sdk', 'piano', 'calibration'):
        parser.add_argument('--' + flag, required=True, type=Path)
    parser.add_argument('--panel', required=True, choices=OEM_CALIBRATIONS)
    parser.add_argument('--driver-source', type=Path, help='NativeFocusPenPro source checkout')
    parser.add_argument('--linker', type=Path,
                        default=Path.home() / '.local/share/unixnu-startup-lto/toolchain-gpu/bin/ld')
    parser.add_argument('--ldid', type=Path, default=Path.home() / '.cache/ldid/ldid')
    args = parser.parse_args()
    core = DEVICE.parent / 'UniXNU'
    driver = (args.driver_source or core / 'drivers/NativeFocusPenPro').resolve()
    out = args.output.resolve()
    source = out / 'piano-source'
    prepare_decoder(args.piano, source)
    profile = compact_calibration(args.calibration.read_bytes(), args.panel)
    (out / 'calibration.ini').write_bytes(profile)
    binary = out / 'uke-pen-hover'
    c_object = out / 'pen-frame.o'
    common = ['-target', 'arm64e-apple-ios27.0', '-isysroot', str(args.sdk.resolve()),
              '-O2', '-Wall', '-Wextra', '-Werror']
    subprocess.run(['clang', *common, '-c', source / 'piano-pen-frame.c', '-o', c_object], check=True)
    panel_flags = ['-DUKE_PEN_CSOT_EDGE_CALIBRATION=1'] if args.panel == 'CSOT' else []
    subprocess.run(['clang++', *common, *panel_flags, '-std=c++20', '-I', source,
                    f'--ld-path={args.linker.resolve()}', '-Wl,-fixup_chains',
                    driver / 'UkePenHover.cpp', driver / 'UkePenReports.cpp',
                    source / 'piano-pen-core.cpp', source / 'piano-pen-decoder.cpp', c_object,
                    '-framework', 'IOKit', '-framework', 'CoreFoundation',
                    '-o', binary], check=True)
    entitlements = out / 'hover.entitlements.plist'
    entitlements.write_bytes(plistlib.dumps(ENTITLEMENTS))
    subprocess.run([args.ldid, '-Hsha256', '-P12', '-Cadhoc', '-S' + str(entitlements), '-I' + LABEL, binary], check=True)
    signing = runpy.run_path(str(DEVICE / 'scripts/build-backlight-runtime.py'))
    signature = signing['signature'](binary.read_bytes())
    signing['require_adhoc']({LABEL: signature})
    if signature['identifier'] != LABEL or signature['entitlements'] != ENTITLEMENTS:
        raise ValueError('unexpected hover service signature')

    addon = runpy.run_path(str(core / 'scripts/addon-add-dir.py'))
    base = {name: (mode, data) for name, mode, data in addon['read'](args.addon)}
    path = 'xpc/launchd.plist'
    cache = runpy.run_path(str(DEVICE / 'scripts/build-icon-service-cache.py'))
    cache['verify_signature'](base[path][1], base[path + '.sig'][1])
    config = plistlib.loads(base[path][1])
    if config['VersionNumber'] != 7 or JOB in config['LaunchDaemons'] or BINARY in base or PROFILE in base:
        raise ValueError('expected the Uke radio addon without the hover service')
    job = {'Label': LABEL, 'ProgramArguments': ['/usr/sbin/uke-pen-hover', '/private/var/ngpu/' + PROFILE],
           'RunAtLoad': True, 'UserName': 'root', 'ProcessType': 'Interactive', 'Umask': 0o77}
    config['LaunchDaemons'][JOB] = job
    updated = plistlib.dumps(config, fmt=plistlib.FMT_BINARY, sort_keys=False)
    detached = cache['sign_data'](updated, args.stock_cache_signature.read_bytes(), LABEL + '.launchd-cache')
    cache_hash = cache['verify_signature'](updated, detached)
    extend = runpy.run_path(str(DEVICE / 'scripts/build-pencil-runtime.py'))['extend_trust']
    trust = extend(args.trust.read_bytes(), {signature['cdhash']: 5, cache_hash: 0}, label=LABEL)
    (out / 'owner-trust.bundle').write_bytes(trust)
    merged = dict(base)
    merged.update({BINARY: (0o755, binary.read_bytes()), PROFILE: (0o644, profile),
                   path: (base[path][0], updated), path + '.sig': (base[path + '.sig'][0], detached)})
    for name in ('Apache-2.0.txt', 'OPEN_CORE_ORIGIN.txt'):
        merged['pencil-pairing/legal/' + name] = (0o644, (source / name).read_bytes())
    target = out / 'gpuaddon.bin'
    addon['write'](target, [(name, mode, data) for name, (mode, data) in merged.items()])
    if {name: (mode, data) for name, mode, data in addon['read'](target)} != merged:
        raise ValueError('hover addon round trip differs')
    report = dict(hover_cdhash=signature['cdhash'], cache_cdhash=cache_hash, panel=args.panel,
                  calibration_sha256=sha(profile), addon_sha256=sha(target.read_bytes()), trust_sha256=sha(trust),
                  piano_sources=PIANO_INPUTS, hardware_tested=False)
    (out / 'hover-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
