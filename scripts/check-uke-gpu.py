#!/usr/bin/env python3
"""Check the uke Adreno 732 board values against a device DT and the vendor firmware.

The native GPU driver still carries Adreno 740 constants. This check keeps the
uke-side facts honest and prints the exact remaining inputs:

    python3 scripts/check-uke-gpu.py --dtb /path/to/uke-live.dtb \
        --firmware-dir /path/to/vendor/firmware

Without --dtb or --firmware-dir it validates the board.toml values alone.
It never touches MMIO and never enables CONFIG_XNU_NATIVE_ADRENO.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import tomllib


def cells(path, node, prop, kind='x'):
    output = subprocess.check_output(['fdtget', '-t', kind, str(path), node, prop], text=True).split()
    return [int(value, 16) if kind == 'x' else int(value) for value in output]


def one(path, node, prop, kind='x'):
    value = cells(path, node, prop, kind)
    if len(value) != 1:
        raise SystemExit(f'{node}:{prop} has {len(value)} cells, expected 1')
    return value[0]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dtb', type=Path, help='device DT dump (uke-live.dtb)')
parser.add_argument('--firmware-dir', type=Path, help='directory holding the vendor GPU firmware')
parser.add_argument('--json', type=Path, help='write the report here')
args = parser.parse_args()

board = tomllib.loads((ROOT / 'board.toml').read_text())['gpu']
report = dict(model=board['model'], chip_id=board['chip_id'], compatible=board['compatible'],
              family=board['family'], dt_checked=False, firmware_checked=False,
              inputs_checked=False, firmware=dict(sqe=None, gmu=None, zap=None),
              missing=[], errors=[], nativeno=False)
errors = []


def expect(actual, wanted, what):
    if actual != wanted:
        errors.append(f'{what}: device {actual!r} != board {wanted!r}')


if args.dtb:
    dtb = args.dtb.resolve()
    if not dtb.is_file():
        raise SystemExit(f'{dtb} is not a file')
    gpu = '/soc/qcom,kgsl-3d0@3d00000'
    gmu = '/soc/qcom,gmu@3d69000'
    smmu = '/soc/qcom,kgsl-iommu@3da0000'
    expect(one(dtb, gpu, 'qcom,chipid'), int(board['chip_id'], 0), 'qcom,chipid')
    model = subprocess.check_output(['fdtget', '-t', 's', str(dtb), gpu, 'qcom,gpu-model'], text=True).strip()
    expect(model, board['model'], 'qcom,gpu-model')
    compatible = subprocess.check_output(['fdtget', '-t', 's', str(dtb), gpu, 'compatible'], text=True).split()
    if board['compatible'] not in compatible:
        errors.append(f'compatible: {compatible} lacks {board["compatible"]}')
    reg = cells(dtb, gpu, 'reg')
    expect(reg[0], int(board['gpu_base'], 0), 'GPU reg base')
    expect(reg[1], int(board['gpu_size'], 0), 'GPU reg size')
    gmu_reg = cells(dtb, gmu, 'reg')
    expect(gmu_reg[0], int(board['gmu_base'], 0), 'GMU reg base')
    expect(gmu_reg[1], int(board['gmu_size'], 0), 'GMU reg size')
    expect(gmu_reg[2], int(board['gmu_pdc_base'], 0), 'GMU PDC reg base')
    expect(gmu_reg[3], int(board['gmu_pdc_size'], 0), 'GMU PDC reg size')
    smmu_reg = cells(dtb, smmu, 'reg')
    expect(smmu_reg[0], int(board['smmu_base'], 0), 'SMMU reg base')
    expect(smmu_reg[1], int(board['smmu_size'], 0), 'SMMU reg size')
    user_iommu = cells(dtb, smmu + '/gfx3d_user', 'iommus')
    expect(user_iommu[1], board['sid_user'], 'gfx3d_user SID')
    secure_iommu = cells(dtb, smmu + '/gfx3d_secure', 'iommus')
    expect(secure_iommu[1], board['sid_secure'], 'gfx3d_secure SID')
    gmu_iommu = cells(dtb, gmu, 'iommus')
    expect(gmu_iommu[1], board['sid_gmu'], 'GMU SID')
    report['dt_checked'] = not errors

if args.firmware_dir:
    directory = args.firmware_dir.resolve()
    for role in ('sqe', 'gmu', 'zap'):
        found = None
        for name in board[f'firmware_{role}']:
            path = directory / name
            if path.is_file():
                found = dict(name=name, bytes=path.stat().st_size,
                             sha256=hashlib.sha256(path.read_bytes()).hexdigest())
                break
        report['firmware'][role] = found
        if found:
            pin = board['firmware_pins'].get(found['name'])
            if not pin:
                errors.append(f'{role} firmware {found["name"]} has no pin')
            elif pin['bytes'] != found['bytes'] or pin['sha256'] != found['sha256']:
                errors.append(f'{role} firmware {found["name"]} does not match its pin')
        if not found:
            report['missing'].append(role)
    report['firmware_checked'] = not report['missing']

# The recorded live-DT facts and the A732 catalog are the inputs the build
# turns into headers. Keep board.toml and that record from drifting apart.
inputs_path = ROOT / 'scripts/data/uke-gpu-inputs.json'
if inputs_path.is_file():
    inputs = json.loads(inputs_path.read_text())
    expect(int(inputs['chip']['chip_id'], 0), int(board['chip_id'], 0), 'recorded chip_id')
    expect(inputs['chip']['model'], board['model'], 'recorded model')
    expect(inputs['catalog']['gmem_bytes'], board['gmem_bytes'], 'recorded GMEM')
    expect(inputs['catalog']['gmu_chip_id'], board['gmu_chip_id'], 'recorded GMU chip id')
    expect(inputs['catalog']['gmu_cgc_mode'], board['gmu_cgc_mode'], 'recorded GMU CGC mode')
    expect(inputs['catalog']['preempt_record_bytes'], board['preempt_record_bytes'],
           'recorded preemption record')
    windows = {entry['name']: entry for entry in inputs['dt']['windows']}
    for name, base_key, size_key in (
            ('kgsl_3d0_reg_memory', 'gpu_base', 'gpu_size'),
            ('gmu', 'gmu_base', 'gmu_size'),
            ('gmu_pdc', 'gmu_pdc_base', 'gmu_pdc_size'),
            ('rscc', None, None), ('cx_misc', None, None)):
        if name not in windows:
            errors.append(f'recorded windows lack {name}')
            continue
        if base_key:
            expect(windows[name]['base'], int(board[base_key], 0), f'recorded {name} base')
            expect(windows[name]['bytes'], int(board[size_key], 0), f'recorded {name} size')
    streams = inputs['dt']['streams']
    expect(streams['user'], board['sid_user'], 'recorded user SID')
    expect(streams['secure'], board['sid_secure'], 'recorded secure SID')
    expect(streams['gmu'], board['sid_gmu'], 'recorded GMU SID')
    if len(inputs['registers']['rsc_setup']) != 19:
        errors.append('recorded RSC/PDC sequence is not 19 writes')
    if int(inputs['registers']['core_gpu_delta']) != int(board['gmu_base'], 0) - int(board['gpu_base'], 0):
        errors.append('recorded GMU/GPU delta differs from board.toml')
    for role in ('sqe', 'gmu', 'zap'):
        pin = inputs['firmware'][role]
        names = board[f'firmware_{role}']
        if pin['name'] not in names:
            errors.append(f'{role} firmware name is not a board.toml candidate')
        elif board['firmware_pins'].get(pin['name'], {}).get('sha256') != pin['sha256']:
            errors.append(f'{role} firmware pin differs from board.toml')
    report['inputs_checked'] = not any(error.startswith('recorded') or
                                       error.endswith('sequence is not 19 writes') or
                                       'pin differs' in error for error in errors)

report['driver_state'] = ('NativeAdreno A732 profile, reg-name window selection and generated '
                          'GMU/vote headers; on-device GMU/CP/SMMU bring-up pending')
report['errors'] = errors
if args.json:
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + '\n')

print(f"{board['model']} {board['chip_id']} {board['compatible']} ({board['family']})")
if args.dtb:
    print('DT: ' + ('matches board.toml' if not errors else 'MISMATCH'))
if args.firmware_dir:
    for role in ('sqe', 'gmu', 'zap'):
        item = report['firmware'][role]
        print(f'firmware {role}: ' + ('missing' if not item else f'{item["name"]} {item["bytes"]} B {item["sha256"]}'))
print('recorded inputs: ' + ('match board.toml' if report['inputs_checked'] else 'absent'))
if errors:
    for error in errors:
        print('ERROR ' + error)
    raise SystemExit(1)
print('Board values and recorded A732 inputs OK; the GPU bring-up is still device-unqualified')
