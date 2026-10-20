#!/usr/bin/env python3
"""Build the sheng UEFI candidate in a stock-aligned Android boot v4 container.

The stock sheng boot image uses header version 4 (page size fixed at 4096,
no kernel cmdline in boot.img, no appended DTB; the DTB comes from the
on-device vendor_boot partition and receives ABL's runtime memory fixups).
This script first runs the existing package-sheng-boot.py so the real shim
still passes its QEMU copy/in-place fixtures, then wraps the identical
gzip(BootShim + FD) payload in a v4 header whose fixed fields match the
stock header byte for byte.

Evidence: build/sheng/boot/report-v4.json (ignored, like all build output).
No bootloader acceptance is claimed; hardware boot remains untested.
"""
import gzip
import hashlib
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sheng/boot'
STOCK = ROOT / '.cache/firmware/sheng/porting-inputs/boot.img'

BOOT_ARGS_SIZE = 512
BOOT_EXTRA_ARGS_SIZE = 1024
V4_HEADER_SIZE = 1584          # matches stock boot.img header_size field
V4_PAGE = 4096


def digest(data):
    return hashlib.sha256(data).hexdigest()


def parse_stock_header():
    data = STOCK.read_bytes()
    assert data[:8] == b'ANDROID!', 'stock boot.img magic'
    (kernel_size, ramdisk_size, os_version, header_size) = struct.unpack_from('<4I', data, 8)
    (header_version,) = struct.unpack_from('<I', data, 0x28)
    signature_size = struct.unpack_from('<I', data, V4_HEADER_SIZE - 4)[0]
    cmdline = data[44:44 + BOOT_ARGS_SIZE + BOOT_EXTRA_ARGS_SIZE].split(b'\0')[0]
    assert header_version == 4 and header_size == V4_HEADER_SIZE, 'unexpected stock header geometry'
    return {
        'kernel_size': kernel_size, 'ramdisk_size': ramdisk_size, 'os_version': os_version,
        'header_size': header_size, 'header_version': header_version,
        'signature_size': signature_size, 'cmdline': cmdline.decode(),
        'reserved': list(struct.unpack_from('<4I', data, 24)),
    }


def build_v4_header(kernel_size, stock):
    """Fixed fields mirror stock; only kernel_size describes our payload."""
    header = bytearray(V4_HEADER_SIZE)
    header[0:8] = b'ANDROID!'
    struct.pack_into('<4I', header, 8, kernel_size, stock['ramdisk_size'],
                     stock['os_version'], stock['header_size'])
    struct.pack_into('<4I', header, 24, *stock['reserved'])
    struct.pack_into('<I', header, 0x28, 4)
    # cmdline: stock keeps boot.img cmdline empty (vendor_boot carries it)
    header[44:44 + len(stock['cmdline'])] = stock['cmdline'].encode()
    struct.pack_into('<I', header, V4_HEADER_SIZE - 4, stock['signature_size'])
    return bytes(header)


def main():
    # Rebuild shim + FD artifacts and rerun the QEMU shim fixtures via the
    # existing v0 recipe, which remains the shim's validation entry point.
    subprocess.run(['python3', ROOT / 'scripts/package-sheng-boot.py'], check=True)
    config_fd_size = 0x300000

    fd = (ROOT / 'uefi-sheng/Build/shengPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd').read_bytes()
    if len(fd) != config_fd_size:
        raise SystemExit('FD size differs from sheng configuration')
    shim = (OUT / 'shim.bin').read_bytes()
    combined = shim + fd
    kernel = gzip.compress(combined, mtime=0)

    stock = parse_stock_header()
    header = build_v4_header(len(kernel), stock)
    pages = (len(kernel) + V4_PAGE - 1) // V4_PAGE
    image = header + b'\0' * (V4_PAGE - V4_HEADER_SIZE) + kernel \
        + b'\0' * (pages * V4_PAGE - len(kernel))
    (OUT / 'sheng-uefi-v4.img').write_bytes(image)

    # Round-trip: parse our own image back, then compare the payload.
    parsed = parse_image(image)
    if parsed['kernel'] != kernel:
        raise SystemExit('v4 container round-trip failed')
    decompressed = gzip.decompress(parsed['kernel'])
    if decompressed != combined:
        raise SystemExit('v4 payload round-trip failed')
    if struct.unpack_from('<QQ', decompressed, 8) != (0xA7100000, len(shim) + len(fd)):
        raise SystemExit('shim ARM64 image_size does not cover shim + FD')

    report = {
        'container_sha256': digest(image), 'container_bytes': len(image),
        'kernel_gzip_sha256': digest(kernel), 'payload_sha256': digest(combined),
        'fd_sha256': digest(fd), 'shim_sha256': digest(shim), 'shim_bytes': len(shim),
        'stock_header': stock,
        'field_alignment': {
            'ramdisk_size': 'stock', 'os_version': 'stock', 'header_size': 'stock',
            'reserved': 'stock', 'header_version': 4, 'cmdline': 'stock',
            'signature_size': 'stock', 'page_size': 4096,
            'differences': ['kernel_size describes gzip(BootShim+FD)'],
        },
        'dtb_source': 'on-device vendor_boot (ABL runtime fixups); none appended',
        'qemu': json.loads((OUT / 'report.json').read_text())['qemu'],
        'hardware_tested': False, 'bootloader_acceptance_proven': False,
    }
    (OUT / 'report-v4.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in
                      ('container_bytes', 'container_sha256', 'kernel_gzip_sha256',
                       'payload_sha256', 'stock_header', 'field_alignment')}, indent=2))


def parse_image(image):
    if image[:8] != b'ANDROID!':
        raise SystemExit('bad magic')
    (kernel_size, ramdisk_size, _os, header_size) = struct.unpack_from('<4I', image, 8)
    (header_version,) = struct.unpack_from('<I', image, 0x28)
    signature_size = struct.unpack_from('<I', image, V4_HEADER_SIZE - 4)[0]
    if header_version != 4 or header_size != V4_HEADER_SIZE:
        raise SystemExit('unexpected header geometry')
    kernel = image[V4_PAGE:V4_PAGE + kernel_size]
    expected = V4_PAGE + ((kernel_size + V4_PAGE - 1) // V4_PAGE) * V4_PAGE \
        + ((ramdisk_size + V4_PAGE - 1) // V4_PAGE) * V4_PAGE \
        + ((signature_size + V4_PAGE - 1) // V4_PAGE) * V4_PAGE
    if len(image) != expected:
        raise SystemExit(f'image length {len(image)} != expected {expected}')
    return {'kernel': kernel, 'ramdisk_size': ramdisk_size, 'signature_size': signature_size}


if __name__ == '__main__':
    main()
