#!/usr/bin/env python3
"""Build the locked Linux QMI codec/descriptors for read-only board queries."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--target', default='arm64e-apple-ios27.0.0')
    args = parser.parse_args()
    module, output = args.module.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(['python3', str(module / 'tools/stage-wcn6750-qmi.py')], check=True)
    clang = subprocess.check_output(['xcrun', '-f', 'clang'], text=True).strip()
    resource = subprocess.check_output([clang, '-print-resource-dir'], text=True).strip() + '/include'
    flags = ['-target', args.target, '-mkernel', '-O2', '-g0', '-ffreestanding', '-fno-builtin', '-fno-common',
             '-fno-strict-aliasing', '-fwrapv', '-fno-stack-protector', '-mgeneral-regs-only', '-std=gnu11',
             '-Wno-error', '-Wno-incompatible-pointer-types', '-w', '-DCONFIG_ATH11K=1', '-DCONFIG_ATH11K_AHB=1',
             '-nostdinc', '-isystem', str(module / 'shim/libc'), '-isystem', resource, '-include', 'lx_pci.h']
    for include in ['shim/include', 'include', 'out/pinned', 'out/ath11k-src', 'out']:
        flags += ['-I', str(module / include)]
    sources = [module / 'deps/linux/drivers/soc/qcom/qmi_encdec.c', module / 'out/wcn6750-qmi/Wcn6750Qmi.c']
    raw_objects = []
    for source in sources:
        obj = output / (source.stem + '.o')
        subprocess.run([clang, *flags, '-c', str(source), '-o', str(obj)], check=True)
        raw_objects.append(obj)
    # The component's ordinary full runtime is not linked by this diagnostic.
    # Its codec globals and three support hooks have private kernel names.
    rename = {'_lx_alloc': '_uke_wlfw_alloc', '_lx_free': '_uke_wlfw_free', '_lx_log': '_uke_wlfw_log'}
    for obj in raw_objects:
        for line in subprocess.check_output(['llvm-nm', '-g', '--defined-only', str(obj)], text=True).splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[2].startswith('_qmi_'):
                rename[fields[2]] = '_uke_wlfw' + fields[2]
    mapping = output / 'rename.txt'
    mapping.write_text(''.join(f'{before} {after}\n' for before, after in sorted(rename.items())))
    for obj in raw_objects:
        subprocess.run(['llvm-objcopy', '--redefine-syms=' + str(mapping), str(obj)], check=True)
    allowed = {'_uke_wlfw_alloc', '_uke_wlfw_free', '_uke_wlfw_log', '_memcpy', '_memset', '_strlen'}
    defined, undefined = set(), set()
    for obj in raw_objects:
        for line in subprocess.check_output(['llvm-nm', '-g', str(obj)], text=True).splitlines():
            fields = line.split()
            if len(fields) == 2 and fields[0] == 'U':
                undefined.add(fields[1])
            elif len(fields) == 3:
                defined.add(fields[2])
    missing = undefined - defined - allowed
    if missing:
        raise SystemExit('Unexpected codec imports: ' + ', '.join(sorted(missing)))
    identity = json.loads((module / 'out/wcn6750-qmi/identity.json').read_text())
    identity.update(target=args.target, objects={str(obj): hashlib.sha256(obj.read_bytes()).hexdigest() for obj in raw_objects},
                    native_imports=sorted(undefined - defined), full_ath11k_backend_linked=False, hardware_verified=False)
    (output / 'build-report.json').write_text(json.dumps(identity, indent=2) + '\n')
    for obj in raw_objects:
        print('OBJ ' + str(obj))


if __name__ == '__main__':
    main()
