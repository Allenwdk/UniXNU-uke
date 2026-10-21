#!/usr/bin/env python3
"""Build the raw image flashed to the uke `userdata` partition: the unchanged iPadOS OS and
SystemOS disk images, byte for byte, behind a small manifest.

macOS attaches the dmg with hdiutil; Linux converts it to a raw image with
qemu-img (the same result without a mount).

Layout (all offsets from the first byte of the image, which is the first byte of `userdata`):

    0x000000  manifest block (4096 bytes, see below), then zero padding to 1 MiB
    0x100000  window "OS"        the whole attached OS disk image (GPT + APFS), 1 MiB aligned
    ....      window "SystemOS"  the whole attached SystemOS disk image, 1 MiB aligned

Manifest (little endian): "SHNGROOT", u32 version=1, u32 count, u64 image_bytes, then `count`
128-byte entries {char name[32], u64 offset, u64 size, u32 block_size=512, u32 flags(1=read-only),
u8 sha256[32], 40 reserved bytes}, then at byte 4064 the SHA-256 of bytes 0..4063.

Nothing is flashed here. The image is written under build/ (ignored by Git), the source dmg
SHA-256s are checked against the recorded IPSW extraction, the image is refused if it exceeds the
`userdata` partition read from the tablet's GPT, and the report lists the
hashes. See docs/native-ufs.md for the flash command and the GPT checks around it.
"""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
VOLUMES = ROOT / '.cache/firmware/target-volumes'
OUT = ROOT / 'build/userdata-root'
MIB = 1024 * 1024
# LUN 0 GPT entry "userdata", read from the uke tablet: LBAs 0x2d3000..0x3b147fa.
USERDATA_BYTES = (0x3b147fa - 0x2d3000 + 1) * 4096
WINDOWS = (('OS', '044-76040-038.dmg'), ('SystemOS', '044-76034-038.dmg'))
CHUNK = 8 * MIB


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(CHUNK), b''):
            digest.update(block)
    return digest.hexdigest()


def attach(dmg):
    """Attach read-only without mounting; return (whole-disk raw device, bytes)."""
    info = subprocess.run(['hdiutil', 'imageinfo', '-plist', str(dmg)], capture_output=True, check=True).stdout
    import plistlib
    total = plistlib.loads(info)['Size Information']['Total Bytes']
    out = subprocess.run(['hdiutil', 'attach', '-nomount', '-readonly', '-noverify', '-noautofsck', str(dmg)],
                         capture_output=True, text=True, check=True).stdout
    whole = None
    for line in out.splitlines():
        match = re.match(r'(/dev/disk\d+)\s', line)
        if match and whole is None:
            whole = match.group(1)
    if not whole:
        raise SystemExit('Could not find the attached disk device')
    return whole, total


def detach(device):
    subprocess.run(['hdiutil', 'detach', device], capture_output=True)


def is_udif(dmg):
    with open(dmg, 'rb') as handle:
        handle.seek(-512, 2)
        return handle.read(4) == b'koly'


def image_size(dmg):
    if shutil.which('hdiutil'):
        info = subprocess.run(['hdiutil', 'imageinfo', '-plist', str(dmg)], capture_output=True, check=True).stdout
        import plistlib
        return plistlib.loads(info)['Size Information']['Total Bytes']
    if is_udif(dmg):
        listing = subprocess.run(['7z', 'l', '-slt', str(dmg)], capture_output=True, text=True).stdout
        match = re.search(r'^unpack-size:\s*(\d+)$', listing, re.M)
        if not match:
            raise SystemExit(f'Cannot read the disk size from {dmg}')
        return int(match.group(1))
    # The SystemOS member is a bare APFS container, not a GPT disk image.
    with open(dmg, 'rb') as handle:
        handle.seek(32)
        if handle.read(4) != b'NXSB':
            raise SystemExit(f'{dmg} is neither a UDIF image nor a raw APFS container')
    return dmg.stat().st_size


@contextlib.contextmanager
def raw_image(dmg, label, size, workdir):
    """A readable raw whole-disk image: hdiutil's rdisk, a 7z Dmg stream, or the bare APFS file."""
    if shutil.which('hdiutil'):
        device, _ = attach(dmg)
        try:
            yield device.replace('/dev/disk', '/dev/rdisk')
        finally:
            detach(device)
    elif is_udif(dmg):
        # 7-Zip's Dmg handler decodes the LZFSE chunks and writes the whole
        # GPT+APFS stream. It reports a CRC warning on the Apple images and
        # still produces the correct size; require the GPT signature.
        raw = workdir / (label + '.raw')
        try:
            with raw.open('wb') as target:
                result = subprocess.run(['7z', 'x', '-tDmg', '-so', str(dmg)], stdout=target,
                                        stderr=subprocess.PIPE, text=True)
            if raw.stat().st_size != size:
                raise SystemExit(f'7z produced {raw.stat().st_size} bytes, expected {size}: {result.stderr[-400:]}')
            with raw.open('rb') as handle:
                handle.seek(512)
                if handle.read(8) != b'EFI PART':
                    raise SystemExit('the 7z stream has no GPT header')
            yield str(raw)
        finally:
            raw.unlink(missing_ok=True)
    else:
        yield str(dmg)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, default=OUT / 'userdata-root.img')
    parser.add_argument('--volumes', type=Path, default=VOLUMES, help='directory holding the two dmg members')
    parser.add_argument('--dry-run', action='store_true', help='check sizes and source hashes only')
    args = parser.parse_args()

    sources = []
    for name, member in WINDOWS:
        dmg = args.volumes / member
        record = json.loads(dmg.with_name(dmg.name + '.json').read_text())
        sources.append((name, dmg, record))
    layout, cursor = [], MIB
    for name, dmg, record in sources:
        size = image_size(dmg)
        layout.append((name, cursor, size))
        cursor += (size + MIB - 1) // MIB * MIB
    image_bytes = cursor
    print(f'image {image_bytes} bytes ({image_bytes / 2**30:.2f} GiB); userdata holds {USERDATA_BYTES} bytes')
    if image_bytes > USERDATA_BYTES:
        raise SystemExit('Refusing: the image is larger than the userdata partition')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for name, dmg, record in sources:
        got = sha256_file(dmg)
        if got != record['sha256']:
            raise SystemExit(f'Refusing: {dmg.name} differs from the recorded IPSW extraction')
        print(f'{name}: source {dmg.name} sha256 {got} matches the IPSW record')
    if args.dry_run:
        print('dry run: nothing written')
        return

    entries = []
    with open(args.output, 'wb') as out:
        out.truncate(image_bytes)
        for (name, offset, size), (_, dmg, record) in zip(layout, sources):
            with raw_image(dmg, name, size, args.output.parent) as raw:
                digest = hashlib.sha256()
                out.seek(offset)
                done = 0
                with open(raw, 'rb', buffering=0) as handle:
                    while done < size:
                        want = min(CHUNK, size - done)
                        data = handle.read(want)
                        if len(data) != want:
                            raise SystemExit(f'Short read from {raw} at {done}')
                        digest.update(data)
                        out.write(data)
                        done += want
            entries.append((name, offset, size, digest.digest()))
            print(f'{name}: {size} bytes at {offset:#x} sha256 {digest.hexdigest()}')
        header = bytearray(4096)
        header[0:8] = b'SHNGROOT'
        struct.pack_into('<IIQ', header, 8, 1, len(entries), image_bytes)
        for index, (name, offset, size, digest) in enumerate(entries):
            struct.pack_into('<32sQQII32s40s', header, 24 + 128 * index, name.encode(), offset, size, 512, 1, digest, bytes(40))
        header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
        out.seek(0)
        out.write(header)
    report = {'image': str(args.output.relative_to(ROOT)) if args.output.is_relative_to(ROOT) else args.output.name,
              'image_bytes': image_bytes, 'image_sha256': sha256_file(args.output),
              'manifest_sha256': hashlib.sha256(bytes(header)).hexdigest(),
              'windows': [{'name': n, 'offset': o, 'size': s, 'sha256': d.hex()} for n, o, s, d in entries],
              'userdata_bytes': USERDATA_BYTES, 'flash_command': f'fastboot flash userdata {args.output.name}'}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
