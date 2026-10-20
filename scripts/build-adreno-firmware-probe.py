#!/usr/bin/env python3
"""Build an opt-in native-XNU GMU planner fixture; firmware remains ignored data."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, ROOT / 'scripts/fetch-adreno-inputs.py'], check=True)
out = ROOT / 'build/adreno-firmware-probe'
out.mkdir(parents=True, exist_ok=True)
firmware = ROOT / '.cache/firmware/sheng-gpu/qcom/gmu_gen70200.bin'
data = firmware.read_bytes()
digest = hashlib.sha256(data).digest()
pins = json.loads((ROOT / 'scripts/data/adreno-inputs.json').read_text())['firmware']['files']
sqe = ROOT / '.cache/firmware/sheng-gpu/qcom/a740_sqe.fw'
zap = ROOT / '.cache/firmware/sheng-gpu/qcom/sm8550/sheng/a740_zap.mbn'
embedded = {}
for name, path in (('sqe', sqe), ('zap', zap)):
    key = 'qcom/a740_sqe.fw' if name == 'sqe' else 'qcom/sm8550/sheng/a740_zap.mbn'
    blob = path.read_bytes()
    if hashlib.sha256(blob).hexdigest() != pins[key]['sha256'] or len(blob) != pins[key]['bytes']:
        raise SystemExit('Pinned ' + name + ' image identity differs')
    embedded[name] = (path, blob, pins[key]['sha256'])
(out / 'AdrenoFirmwareInput.h').write_text('#define ADRENO_FIRMWARE_INPUT_BYTES ' + str(len(data)) + '\n' +
    'static const uint8_t expected_gmu_sha256[32] = {' + ','.join(hex(b) for b in digest) + '};\n')
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
objects = []
for source in (ROOT / 'kits/AdrenoFirmware/AdrenoFirmware.c', ROOT / 'kits/uke/AdrenoFirmwareProbe.c'):
    obj = out / (source.stem + '.o')
    command = []
    i = 0
    while i < len(record['arguments']):
        value = record['arguments'][i]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ'):
            i += 1
        elif value == '-Xclang' and record['arguments'][i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        i += 1
    command += ['-I', str(out), '-I', str(ROOT / 'kits/AdrenoFirmware'), '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
    subprocess.run(command, cwd=record['directory'], check=True)
    objects.append(obj)
assembly = out / 'firmware.S'
source = ['.section __DATA_CONST,__const', '.p2align 12',
          '.globl _native_adreno_firmware_image', '_native_adreno_firmware_image:',
          '.incbin ' + json.dumps(str(firmware)), 'Lend:', '.p2align 3',
          '.globl _native_adreno_firmware_image_size', '_native_adreno_firmware_image_size:',
          '.quad Lend-_native_adreno_firmware_image']
for name, (path, blob, expected) in embedded.items():
    label = '_native_adreno_' + name + '_firmware_image'
    source += ['.p2align 12', '.globl ' + label, label + ':',
               '.incbin ' + json.dumps(str(path)),
               'L' + name + 'end:', '.p2align 3',
               '.globl ' + label + '_size', label + '_size:',
               '.quad L' + name + 'end-' + label]
assembly.write_text('\n'.join(source) + '\n')
obj = out / 'firmware.o'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Wno-incompatible-sysroot', '-c', assembly, '-o', obj], check=True)
objects.append(obj)
archive = out / 'libAdrenoFirmwareProbe.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
(out / 'build-report.json').write_text(json.dumps(dict(
    kit_revision=subprocess.check_output(['git', '-C', ROOT / 'kits/AdrenoFirmware', 'rev-parse', 'HEAD'], text=True).strip(),
    firmware_sha256=digest.hex(), firmware_bytes=len(data),
    sqe_sha256=embedded['sqe'][2], sqe_bytes=len(embedded['sqe'][1]),
    zap_sha256=embedded['zap'][2], zap_bytes=len(embedded['zap'][1]),
    archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
    object_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in objects},
    input_source_modified=False, gpu_execution=False, firmware_authenticity_qualified=False), indent=2) + '\n')
print('PASS: native diagnostic archive built from the exact GMU, SQE and ZAP inputs; no firmware or GPU execution')
