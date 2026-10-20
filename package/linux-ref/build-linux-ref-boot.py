#!/usr/bin/env python3
"""Build a fastboot-boot-only Linux reference image for the GPU: the prebuilt sheng kernel (ianchb/sm8550-mainline release asset
boot_sheng_singleboot.img, kernel and appended DTB reused byte for byte) plus a new initramfs holding busybox, the msm-ref replay program,
the init script next to this file and the three GPU firmware files. The command line drops root=PARTLABEL=userdata, so nothing on UFS is
mounted. Inputs and output stay outside Git; the output is for `fastboot boot` only, never for flashing.

usage: build-linux-ref-boot.py --boot boot_sheng_singleboot.img --busybox busybox --msm-ref msm-ref --firmware DIR --out linux-ref.img
                              [--module x.ko ...] [--initrd-out initrd.gz]
Modules (from the same release's linux-xiaomi-sheng.deb, e.g. gpio-shared-proxy.ko, which the panel's backlight needs) are loaded by init.
--initrd-out also writes the initramfs alone, for kexec_file_load from a running reference session. DIR holds qcom/a740_sqe.fw, qcom/gmu_gen70200.bin and qcom/sm8550/sheng/a740_zap.mbn."""
import argparse
import gzip
import hashlib
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIRMWARE = ('qcom/a740_sqe.fw', 'qcom/gmu_gen70200.bin', 'qcom/sm8550/sheng/a740_zap.mbn')
# separate_gpu_kms gives the GPU its own DRM device, so the render node does not wait for every display component (the DisplayPort
# controller waits for pmic_glink, which needs the ADSP; none of that runs here). ABL puts this text in front of its own arguments.
CMDLINE = b'msm.separate_gpu_kms=1 rdinit=/init console=tty0 loglevel=4 consoleblank=0'


def cpio(entries):
    """newc archive; entries are (path, mode, data) with directories first."""
    out = bytearray()
    for ino, (name, mode, data) in enumerate(entries + [('TRAILER!!!', 0, b'')], 1):
        path = name.encode() + b'\0'
        out += b'070701' + b''.join(b'%08x' % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(path), 0)) + path
        out += b'\0' * (-len(out) % 4) + data
        out += b'\0' * (-len(out) % 4)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    for name in ('boot', 'busybox', 'msm-ref', 'firmware', 'out'):
        ap.add_argument('--' + name, required=True, type=Path)
    ap.add_argument('--module', action='append', default=[], type=Path)
    ap.add_argument('--initrd-out', type=Path)
    a = ap.parse_args()
    boot = a.boot.read_bytes()
    if boot[:8] != b'ANDROID!':
        raise SystemExit('not an Android boot image')
    kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr, tags, page, version, os_version = struct.unpack_from('<10I', boot, 8)
    if version != 0 or ramdisk_size or second_size:
        raise SystemExit('expected the header v0 release image without ramdisk')
    kernel = boot[page:page + kernel_size]

    dirs = ['bin', 'dev', 'proc', 'sys', 'run', 'tmp', 'lib', 'lib/firmware', 'lib/firmware/qcom', 'lib/firmware/qcom/sm8550',
            'lib/firmware/qcom/sm8550/sheng', 'lib/modules']
    entries = [(d, 0o40755, b'') for d in dirs]
    entries += [('dev/console', 0o20600, b''), ('init', 0o100755, (HERE / 'init').read_bytes()),
                ('bin/busybox', 0o100755, a.busybox.read_bytes()), ('bin/msm-ref', 0o100755, a.msm_ref.read_bytes())]
    entries += [('lib/firmware/' + f, 0o100644, (a.firmware / f).read_bytes()) for f in FIRMWARE]
    entries += [('lib/modules/' + m.name, 0o100644, m.read_bytes()) for m in a.module]
    ramdisk = gzip.compress(cpio(entries), 9, mtime=0)
    if a.initrd_out:
        a.initrd_out.write_bytes(ramdisk)

    pad = lambda b: b + b'\0' * (-len(b) % page)
    header = bytearray(page)
    header[:8] = b'ANDROID!'
    struct.pack_into('<10I', header, 8, len(kernel), kernel_addr, len(ramdisk), 0x01000000, 0, 0x00f00000, tags, page, 0, os_version)
    header[64:64 + len(CMDLINE)] = CMDLINE
    digest = hashlib.sha1()
    for blob in (kernel, ramdisk, b''):
        digest.update(blob)
        digest.update(struct.pack('<I', len(blob)))
    header[576:596] = digest.digest()
    image = bytes(header) + pad(kernel) + pad(ramdisk)
    a.out.write_bytes(image)
    print(f'BUILT {a.out} ({len(image)} bytes) sha256 {hashlib.sha256(image).hexdigest()} ramdisk {len(ramdisk)} cmdline {CMDLINE.decode()}')


if __name__ == '__main__':
    main()
