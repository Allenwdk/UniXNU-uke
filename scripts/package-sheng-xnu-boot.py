#!/usr/bin/env python3
"""Pack the sheng UEFI candidate and the CrabApple payload into one boot image.

The v0 container written by package-sheng-boot.py carries gzip(BootShim + FD)
plus the appended DTB. This script adds the payload FAT volume (CrabApple
config, DarwinBoot, the 4 KiB source XNU and its ADT) as the image's ramdisk.
The stock ABL copies a v0 ramdisk to memory, records it in /chosen, and the
firmware's ShengBootMemoryDxe publishes it as a read-only block device, so no
flash write is needed: `fastboot boot` only.

Output: build/sheng/boot/sheng-uefi-xnu.img and sheng-uefi-xnu.json (ignored).
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sheng/boot'
KERNEL = OUT / 'Image.gz-dtb'
PAYLOAD = OUT / 'sheng-userdata-payload.img'
IMAGE = OUT / 'sheng-uefi-xnu.img'


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    config = tomllib.loads((ROOT / 'uefi-sheng/Resources/Configs/sheng.toml').read_text())['boot_image']
    for path in (KERNEL, PAYLOAD):
        if not path.is_file():
            raise SystemExit(f'Missing input: {path}')
    size = PAYLOAD.stat().st_size
    if size % 512:
        raise SystemExit('The payload must be a whole number of 512-byte sectors')
    command = ['python3', ROOT / 'uefi-sheng/Resources/Scripts/mkbootimg.py',
               '--kernel', KERNEL, '--ramdisk', PAYLOAD, '-o', IMAGE]
    for key in ('base', 'kernel_offset', 'tags_offset', 'pagesize', 'header_version'):
        command.extend([f'--{key}', str(config[key])])
    subprocess.run(command, check=True)

    with IMAGE.open('rb') as stream:
        header = stream.read(4096)
        kernel_size, kernel_address, ramdisk_size, _, second_size, _, tags, page_size, version, _ = \
            struct.unpack_from('<10I', header, 8)
        if (header[:8] != b'ANDROID!' or ramdisk_size != size or second_size or version != config['header_version']
                or page_size != config['pagesize']):
            raise SystemExit('Boot image header does not describe the kernel and ramdisk')
        kernel_pages = -(-kernel_size // page_size)
        stream.seek(page_size)
        if stream.read(kernel_size) != KERNEL.read_bytes():
            raise SystemExit('Kernel bytes differ')
        stream.seek(page_size * (1 + kernel_pages))
        digest = hashlib.sha256()
        remaining = ramdisk_size
        while remaining:
            chunk = stream.read(min(remaining, 1 << 20))
            if not chunk:
                raise SystemExit('Boot image ends inside the ramdisk')
            digest.update(chunk)
            remaining -= len(chunk)
    payload_hash = sha256(PAYLOAD)
    if digest.hexdigest() != payload_hash:
        raise SystemExit('Ramdisk bytes differ from the payload')
    report = {'image_sha256': sha256(IMAGE), 'image_bytes': IMAGE.stat().st_size,
              'kernel_sha256': sha256(KERNEL), 'payload_sha256': payload_hash, 'payload_bytes': size,
              'header_version': version, 'hardware_tested': False}
    (OUT / 'sheng-uefi-xnu.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
