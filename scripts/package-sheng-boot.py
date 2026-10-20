#!/usr/bin/env python3
"""Build an offline Android container and exercise the real shim in QEMU."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time
import tomllib
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sheng/boot'


def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def abl_dtb():
    # Appended DTB must carry the stock splash_region or ABL powers the panel off.
    subprocess.run(['python3', ROOT / 'scripts/sheng-abl-dtb.py'], check=True)
    return (ROOT / 'build/sheng/sheng-abl.dtb').read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-qemu', action='store_true', help='Package with byte round-trip checks only; record that shim execution was not tested')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    config = tomllib.loads((ROOT / 'uefi-sheng/Resources/Configs/sheng.toml').read_text())
    base, size = (config['uefi_fd'][key] for key in ('base', 'size'))
    if not config['boot_shim']['requires_kernel_header'] or size % 16:
        raise SystemExit('This fixture requires the configured ARM64 header and 16-byte copy size')
    tools = {name: shutil.which(name) for name in ('clang', 'ld.lld', 'llvm-objcopy', 'llvm-nm') + (() if args.skip_qemu else ('qemu-system-aarch64',))}
    if not all(tools.values()):
        raise SystemExit('Put LLVM, lld and QEMU on PATH')
    subprocess.run(['python3', ROOT / 'scripts/check-sheng-fv.py'], check=True)
    fd = (ROOT / 'uefi-sheng/Build/shengPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd').read_bytes()
    if len(fd) != size:
        raise SystemExit('FD size differs from sheng configuration')
    common = [tools['clang'], '--target=aarch64-none-elf', '-c', f'-DFD_BASE={base}', f'-DFD_SIZE={size}']
    run(*common, '-DREQUIRES_KERNEL_HEADER=1', ROOT / 'uefi-sheng/BootShim/BootShim.S', '-o', OUT / 'shim.o')
    run(tools['llvm-objcopy'], '-O', 'binary', OUT / 'shim.o', OUT / 'shim.bin')
    shim = (OUT / 'shim.bin').read_bytes()
    symbols = run(tools['llvm-nm'], '-n', OUT / 'shim.o')
    payload_offset = next(int(line.split()[0], 16) for line in symbols.splitlines() if line.endswith(' _Payload'))
    if payload_offset != len(shim):
        raise SystemExit('Literal pool or padding separates the payload label from the appended FD')
    if shim[56:60] != b'ARM\x64' or struct.unpack_from('<QQ', shim, 8) != (base, len(shim) + size):
        raise SystemExit('ARM64 header does not describe the complete shim + FD')
    results = []
    if not args.skip_qemu:
        fixture = ROOT / 'scripts/qemu-entry/bootshim-fixture.S'
        run(*common, '-DPAYLOAD=1', fixture, '-o', OUT / 'payload.o')
        run(tools['ld.lld'], f'-Ttext={base:#x}', '-e', '_start', OUT / 'payload.o', '-o', OUT / 'payload.elf')
        run(tools['llvm-objcopy'], '-O', 'binary', OUT / 'payload.elf', OUT / 'payload-code.bin')
        code = (OUT / 'payload-code.bin').read_bytes()
        synthetic = bytearray((bytes(range(256)) * ((size + 255) // 256))[:size])
        synthetic[:len(code)] = code
        (OUT / 'fixture.bin').write_bytes(shim + synthetic)
        for name, address in [('copy', 0x50000000), ('already-in-place', base - len(shim))]:
            run(*common, f'-DSHIM_BASE={address}', f'-DSHIM_SIZE={len(shim)}', fixture, '-o', OUT / 'entry.o')
            run(tools['ld.lld'], '-Ttext=0x40080000', '-e', '_start', OUT / 'entry.o', '-o', OUT / 'entry.elf')
            run(tools['llvm-objcopy'], '-O', 'binary', OUT / 'entry.elf', OUT / 'entry.bin')
            log = OUT / f'{name}.uart.log'
            log.unlink(missing_ok=True)
            command = [tools['qemu-system-aarch64'], '-M', 'virt,virtualization=off,secure=off',
                       '-accel', 'tcg', '-cpu', 'cortex-a710', '-m', '2G', '-smp', '1',
                       '-display', 'none', '-monitor', 'none', '-nic', 'none', '-no-reboot',
                       '-serial', f'file:{log}', '-kernel', str(OUT / 'entry.bin'),
                       '-device', f'loader,file={OUT / "fixture.bin"},addr={address:#x},force-raw=on']
            start = time.monotonic()
            with (OUT / f'{name}.stderr.log').open('w') as errors:
                process = subprocess.Popen(command, stdout=errors, stderr=errors)
                try:
                    while process.poll() is None and time.monotonic() - start < 3:
                        if log.exists() and ('PASS' in log.read_text() or 'FAIL' in log.read_text()):
                            break
                        time.sleep(0.02)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
            uart = log.read_text() if log.exists() else ''
            if 'BOOTSHIM: full copy, destination entry and x0 PASS' not in uart or 'FAIL' in uart:
                raise SystemExit(f'{name}: shim fixture failed; inspect {log}')
            results.append({'case': name, 'seconds': round(time.monotonic() - start, 3), 'passed': True})
    dtb = abl_dtb()
    if dtb[:4] != b'\xd0\x0d\xfe\xed' or struct.unpack_from('>I', dtb, 4)[0] != len(dtb):
        raise SystemExit('Invalid compiled sheng DTB')
    combined = shim + fd
    kernel = gzip.compress(combined, mtime=0) + dtb
    (OUT / 'Image.gz-dtb').write_bytes(kernel)
    # No initrd is consumed by the shim/SEC. Avoid the upstream literal "dummy" file.
    (OUT / 'ramdisk').write_bytes(b'')
    image = config['boot_image']
    command = ['python3', ROOT / 'uefi-sheng/Resources/Scripts/mkbootimg.py',
               '--kernel', OUT / 'Image.gz-dtb', '--ramdisk', OUT / 'ramdisk', '-o', OUT / 'sheng-uefi.img']
    for key in ('base', 'kernel_offset', 'tags_offset', 'pagesize', 'header_version'):
        command.extend([f'--{key}', str(image[key])])
    subprocess.run(command, check=True)
    boot = (OUT / 'sheng-uefi.img').read_bytes()
    words = struct.unpack_from('<10I', boot, 8)
    kernel_size, kernel_address, ramdisk_size, _, second_size, _, tags, page_size, version, _ = words
    if (boot[:8] != b'ANDROID!' or kernel_size != len(kernel) or ramdisk_size or second_size
            or version != image['header_version'] or page_size != image['pagesize']
            or kernel_address != image['base'] + image['kernel_offset']
            or tags != image['base'] + image['tags_offset']
            or boot[page_size:page_size + kernel_size] != kernel):
        raise SystemExit('Android container round-trip failed')
    decompressor = zlib.decompressobj(16 + zlib.MAX_WBITS)
    unpacked = decompressor.decompress(boot[page_size:page_size + kernel_size])
    if not decompressor.eof or unpacked != combined or decompressor.unused_data != dtb:
        raise SystemExit('gzip/FD/appended-DTB round-trip failed')
    stock = ROOT / '.cache/firmware/sheng/porting-inputs'
    stock_boot = next(stock.rglob('boot.img'))
    with stock_boot.open('rb') as stream:
        stock_header = stream.read(44)
    if stock_header[:8] != b'ANDROID!':
        raise SystemExit('Unexpected stock boot image magic')
    report = {'fd_sha256': digest(fd), 'shim_sha256': digest(shim), 'shim_bytes': len(shim),
              'image_size': len(combined), 'android_header_version': version,
              'kernel_address_field': hex(kernel_address), 'dtb_sha256': digest(dtb),
              'container_sha256': digest(boot), 'container_bytes': len(boot), 'qemu': results, 'qemu_skipped': args.skip_qemu,
              'hardware_tested': False, 'bootloader_acceptance_proven': False,
              'caches_tested': 'not tested in this package run' if args.skip_qemu else 'MMU/cache-off QEMU fixture only',
              'stock_boot_header_version': struct.unpack_from('<I', stock_header, 40)[0],
              'limitations': ['Legacy v0 recipe follows debian-sheng; stock firmware uses v4',
                              'Stock ABL placement and runtime memory/DT fixups remain unverified']}
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
