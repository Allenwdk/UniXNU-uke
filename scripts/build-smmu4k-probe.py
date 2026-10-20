#!/usr/bin/env python3
"""Build the default-off native-XNU inactive-table/physical-allocation fixture."""
import hashlib
import json
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
out = ROOT / 'build/smmu4k-probe'
out.mkdir(parents=True, exist_ok=True)
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/vm_kern.o.json').read_text().strip().rstrip(','))
objects = []
for source in (ROOT / 'kits/ArmSmmu4K/ArmSmmu4K.c', ROOT / 'kits/uke/ArmSmmu4KProbe.c'):
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
    command += ['-I', str(out), '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(ROOT / 'kits/ArmSmmu4K'), '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
    subprocess.run(command, cwd=record['directory'], check=True)
    objects.append(obj)
archive = out / 'libArmSmmu4KProbe.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
(out / 'build-report.json').write_text(json.dumps(dict(
    kit_revision=subprocess.check_output(['git', '-C', ROOT / 'kits/ArmSmmu4K', 'rev-parse', 'HEAD'], text=True).strip(),
    archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
    object_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in objects},
    gpu_execution=False, submission_authorized=False), indent=2) + '\n')
print('PASS: native SMMU4K diagnostic archive built; no SMMU activation or GPU execution')
