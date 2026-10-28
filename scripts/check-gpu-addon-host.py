#!/usr/bin/env python3
"""Host check for the GPU addon artifact.

Parses build/uke-gpu-addon/gpu-addon.z the way the DarwinBoot loader and
kern_exec.c's native_gpu_addon_install do (NGPUADD2 header, per-file zlib
streams, bounds), validates the tree the kernel will write against the packer
report and the current plugin build, derives each Mach-O's ldid CDHash from
its CodeDirectory, and optionally confirms the owner trust module carries
those CDHashes, that a payload carries this exact blob as
Payloads/kernel.macho.gpuaddon with the ADT GpuAddon entry and the Metal addon
boot arguments, and that a vendor_boot image carries that payload at 0x1000.
Nothing here touches hardware.

  check-gpu-addon-host.py [--blob-dir DIR] [--plugin-dir DIR]
                          [--owner-trust BUNDLE] [--kernel MACHO]
                          [--payload IMG] [--vendor-boot IMG]
"""
import argparse
import hashlib
import json
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct('<8sIIQ')      # magic, entry count, reserved, total file bytes
ENTRY = struct.Struct('<IIQQ')        # path bytes (with NUL), mode, data length, compressed length
MAGIC = b'NGPUADD2'
PATH_MAX = 1024
EXPECTED = {
    'addons/NativeAdrenoMetal.bundle/NativeAdrenoMetal': 0o755,
    'addons/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib': 0o755,
    'addons/NativeAdrenoMetal.bundle/Info.plist': 0o644,
    'addons/NativeAdrenoMetal.publish': 0o644,
    'addons/NativeAdrenoMetal.clients': 0o644,
    'bin/adrenoprobed': 0o755,
    'daemons/org.blu-sharky.adrenoprobed.plist': 0o644,
}


def fail(message):
    sys.exit('FAIL: ' + message)


def fnv1a(data):
    value = 0x811c9dc5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xffffffff
    return value


def cdhash(macho):
    """SHA-256 CodeDirectory digest truncated to 20 bytes (ldid -h semantics)."""
    data = macho.read_bytes()
    if len(data) < 32 or struct.unpack_from('<I', data, 0)[0] != 0xfeedfacf:
        fail(str(macho) + ': not a 64-bit Mach-O')
    ncmds, = struct.unpack_from('<I', data, 16)
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == 0x1d:  # LC_CODE_SIGNATURE
            off, length = struct.unpack_from('<II', data, at + 8)
            if off + length > len(data) or length < 12:
                fail(str(macho) + ': code signature outside the file')
            superblob = data[off:off + length]
            if struct.unpack_from('>I', superblob, 0)[0] != 0xfade0cc0:
                fail(str(macho) + ': signature is not a SuperBlob')
            count, = struct.unpack_from('>I', superblob, 8)
            for index in range(count):
                blob_type, blob_off = struct.unpack_from('>II', superblob, 12 + 8 * index)
                if blob_type != 0:  # kSecCodeMagicCodeDirectory
                    continue
                if blob_off + 8 > len(superblob):
                    fail(str(macho) + ': CodeDirectory index outside the SuperBlob')
                if struct.unpack_from('>I', superblob, blob_off)[0] != 0xfade0c02:
                    fail(str(macho) + ': CodeDirectory magic mismatch')
                blob_len, = struct.unpack_from('>I', superblob, blob_off + 4)
                if blob_off + blob_len > len(superblob):
                    fail(str(macho) + ': CodeDirectory length outside the SuperBlob')
                return hashlib.sha256(superblob[blob_off:blob_off + blob_len]).hexdigest()[:40]
            fail(str(macho) + ': no CodeDirectory in the signature')
        at += size
    fail(str(macho) + ': no LC_CODE_SIGNATURE')


def parse_blob(blob):
    """Parse the NGPUADD2 stream exactly as DarwinBoot checks it and
    kern_exec.c's native_gpu_addon_install validates it: a fixed 24-byte
    header, then per entry u32 path bytes (with NUL), u32 mode, u64 size,
    u64 compressed size, the path padded to 8 and one zlib stream padded to
    8. The header's total size is the whole file."""
    if len(blob) < HEADER.size:
        fail('blob is shorter than its header')
    if blob[:8] != MAGIC:
        fail('blob magic %r is not %r' % (blob[:8], MAGIC))
    count, reserved = struct.unpack_from('<II', blob, 8)
    raw_size = struct.unpack_from('<Q', blob, 16)[0]
    if raw_size != len(blob):
        fail('header total %u differs from the file size %u' % (raw_size, len(blob)))
    if not 0 < count <= 4096:
        fail('entry count %u is outside 1..4096' % count)
    entries = {}
    at = HEADER.size
    for index in range(count):
        if at + ENTRY.size > len(blob):
            fail('entry %u header is truncated' % index)
        path_len, mode, data_len, packed_len = ENTRY.unpack_from(blob, at)
        at += ENTRY.size
        name_padded = (path_len + 7) & ~7
        data_padded = (packed_len + 7) & ~7
        if not 0 < path_len <= PATH_MAX or not data_len <= 0x7fffffff or not packed_len or packed_len > 0x7fffffff:
            fail('entry %u lengths are invalid' % index)
        if at + name_padded + data_padded > len(blob):
            fail('entry %u runs past the blob' % index)
        name = blob[at:at + path_len].split(b'\0', 1)[0].decode('utf-8', 'strict')
        if name in entries:
            fail('duplicate entry ' + name)
        try:
            data = zlib.decompress(blob[at + name_padded:at + name_padded + packed_len])
        except zlib.error as error:
            fail(name + ': zlib stream does not inflate: %s' % error)
        if len(data) != data_len:
            fail('%s: inflated %u bytes, header says %u' % (name, len(data), data_len))
        entries[name] = {'mode': mode, 'data': data}
        at += name_padded + data_padded
    if at != len(blob):
        fail('%u trailing bytes after %u entries' % (len(blob) - at, count))
    return entries, raw_size, 0, len(blob)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--blob-dir', type=Path, default=ROOT / 'build/uke-gpu-addon')
    parser.add_argument('--plugin-dir', type=Path, default=ROOT.parent / 'UniXNU/build/adreno-metal-plugin')
    parser.add_argument('--owner-trust', type=Path, help='owner trust bundle that the payload carries')
    parser.add_argument('--kernel', type=Path, help='prelinked kernel that should embed this blob')
    parser.add_argument('--payload', type=Path, help='payload image whose unixnu.toml should carry the addon arguments')
    parser.add_argument('--vendor-boot', type=Path, help='vendor_boot image that should carry --payload at 0x1000')
    args = parser.parse_args()

    blob = (args.blob_dir / 'gpu-addon.z').read_bytes()
    entries, raw_size, cookie, blob_size = parse_blob(blob)
    print('PASS: blob %u bytes, header total %u, %u entries' % (blob_size, raw_size, len(entries)))

    report_path = args.blob_dir / 'gpu-addon.json'
    if report_path.is_file():
        report = json.loads(report_path.read_text())
        if report.get('blob_sha256') != hashlib.sha256(blob).hexdigest():
            fail('gpu-addon.json blob_sha256 does not match the blob')
        if report.get('bytes') != len(blob):
            fail('gpu-addon.json byte count does not match the blob')
        reported = {entry['path']: entry for entry in report.get('entries', [])}
        if set(reported) != set(entries):
            fail('gpu-addon.json entry list does not match the blob')
        for name, entry in entries.items():
            if reported[name]['sha256'] != hashlib.sha256(entry['data']).hexdigest():
                fail(name + ': sha256 does not match gpu-addon.json')
        print('PASS: gpu-addon.json matches the blob')

    probe = 'addons/NativeAdrenoMetal.publish' not in entries
    expected = dict(EXPECTED)
    if probe:
        del expected['addons/NativeAdrenoMetal.publish']
    wallpaper = {name for name in entries if name.startswith('wallpaper/')}
    extras = wallpaper | ({'addons/NativeAdrenoMetal.generic'} & set(entries))
    if set(entries) - extras != set(expected):
        fail('entry set differs: %s' % sorted((set(entries) - extras) ^ set(expected)))
    for name, mode in expected.items():
        if entries[name]['mode'] & 0o7777 != mode:
            fail('%s: mode %04o, expected %04o' % (name, entries[name]['mode'] & 0o7777, mode))
    for name in wallpaper:
        if entries[name]['mode'] & 0o7777 != 0o644:
            fail('%s: wallpaper file mode %04o, expected 0644' % (name, entries[name]['mode'] & 0o7777))
    print('PASS: the %u expected entries with the expected modes%s%s' % (
        len(expected), ' (probe, no publish marker)' if probe else '',
        ' + %u wallpaper files' % len(wallpaper) if wallpaper else ''))

    if not probe:
        publish = entries['addons/NativeAdrenoMetal.publish']['data'].strip()
        if not publish:
            fail('the publish marker is empty')
    clients = entries['addons/NativeAdrenoMetal.clients']['data'].decode().split()
    if not clients or any('/' in client or not client for client in clients):
        fail('the clients list is empty or malformed')
    for required in ('iconservicesagent', 'WidgetRenderer_Default'):
        if required not in clients:
            fail('the clients list does not name ' + required)
    info = plistlib.loads(entries['addons/NativeAdrenoMetal.bundle/Info.plist']['data'])
    if info.get('CFBundleExecutable') != 'NativeAdrenoMetal' or info.get('NSPrincipalClass') != 'NativeAdrenoMetalDevice':
        fail('Info.plist does not name the bundle executable and principal class')
    if 'addons/NativeAdrenoMetal.bundle/' + info['CFBundleExecutable'] not in entries:
        fail('the Info.plist executable is not in the blob')
    daemon = plistlib.loads(entries['daemons/org.blu-sharky.adrenoprobed.plist']['data'])
    if daemon.get('Label') != 'org.blu-sharky.adrenoprobed':
        fail('the daemon plist label differs from its file name')
    if daemon.get('ProgramArguments') != ['/private/var/ngpu/bin/adrenoprobed']:
        fail('the daemon plist does not run the blob bin/adrenoprobed')
    print('PASS: markers, clients list, Info.plist and daemon plist')

    packaged = {
        'addons/NativeAdrenoMetal.bundle/NativeAdrenoMetal': args.plugin_dir / 'NativeAdrenoMetal.bundle/NativeAdrenoMetal',
        'addons/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib': args.plugin_dir / 'NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib',
        'addons/NativeAdrenoMetal.bundle/Info.plist': args.plugin_dir / 'NativeAdrenoMetal.bundle/Info.plist',
        'bin/adrenoprobed': args.plugin_dir / 'adrenoprobed',
        'daemons/org.blu-sharky.adrenoprobed.plist': args.plugin_dir / 'org.blu-sharky.adrenoprobed.plist',
    }
    for name, source in packaged.items():
        if source.is_file() and entries[name]['data'] != source.read_bytes():
            fail(name + ': the blob is stale against ' + str(source))
    print('PASS: packaged bundle, daemon and plist match the plugin build')

    trust_path = args.plugin_dir / 'trust.json'
    owner_path = args.plugin_dir / 'owner-trust.json'
    if trust_path.is_file():
        trust = json.loads(trust_path.read_text())
        hashes = {
            'addons/NativeAdrenoMetal.bundle/NativeAdrenoMetal': trust['cdhash'],
            'addons/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib': trust['turnip_cdhash'],
            'bin/adrenoprobed': trust['daemon_cdhash'],
        }
        for name, expected in hashes.items():
            actual = cdhash(args.plugin_dir / packaged[name].relative_to(args.plugin_dir))
            if actual != expected:
                fail(name + ': CodeDirectory CDHash %s differs from trust.json %s' % (actual, expected))
        print('PASS: packaged CDHashes match trust.json: %s' % ', '.join(sorted(set(hashes.values()))))
        if owner_path.is_file():
            owner = json.loads(owner_path.read_text())
            if sorted(owner.get('own_cdhashes', [])) != sorted(hashes.values()):
                fail('owner-trust.json does not carry the three packaged CDHashes')
            if args.owner_trust:
                bundle = args.owner_trust.read_bytes()
                for digest in hashes.values():
                    if bytes.fromhex(digest) not in bundle:
                        fail('owner trust %s does not contain %s' % (args.owner_trust, digest))
                if hashlib.sha256(bundle).hexdigest() != owner.get('bundle_sha256'):
                    fail('owner trust bundle sha256 differs from owner-trust.json')
                print('PASS: owner trust %s carries all three CDHashes' % args.owner_trust)

    if args.kernel:
        kernel = args.kernel.read_bytes()
        if b'UKE GPU addon: stage=' not in kernel:
            fail(str(args.kernel) + ': lacks the staging log')
        if b'native_gpu_addon_zalloc' not in kernel:
            fail(str(args.kernel) + ': lacks the zlib allocator wrapper; inflateInit would call NULL on the first boot')
        if kernel.find(blob) >= 0:
            fail(str(args.kernel) + ': still embeds the addon; the loader carries it instead (payload would not fit)')
        print('PASS: %s carries the addon installer (not the blob; the loader hands it over)' % args.kernel)

    if args.payload:
        mcopy = shutil.which('mcopy')
        if not mcopy:
            fail('mcopy (mtools) is needed to read the payload boot configuration')
        try:
            toml = subprocess.run([mcopy, '-i', str(args.payload), '::/EFI/UniXNU/unixnu.toml', '-'],
                                  capture_output=True, check=True).stdout.decode()
        except subprocess.CalledProcessError as error:
            fail('cannot read unixnu.toml from %s: %s' % (args.payload, error.stderr.decode().strip()))
        match = re.search(r'^args = "(.*)"$', toml, re.MULTILINE)
        if not match:
            fail(str(args.payload) + ': unixnu.toml has no xnu args line')
        boot_args = match.group(1)
        if len(boot_args.encode()) > 1000:
            fail('%s: boot args are %u bytes, over the 1000-byte limit' % (args.payload, len(boot_args.encode())))
        tokens = boot_args.split()
        for required in ('sheng-gpu-addon=1', 'sheng-gpu-metal=1'):
            if required not in tokens:
                fail(str(args.payload) + ': boot args lack ' + required)
        tag = next((token.split('=', 1)[1] for token in tokens if token.startswith('sheng-build-tag=')), None)
        surface = next((token.split('=', 1)[1] for token in tokens if token.startswith('offline-legacy-default-surface=')), None)
        fbgpu = next((token.split('=', 1)[1] for token in tokens if token.startswith('sheng-fb-gpu=')), None)
        print('PASS: %s boot args %u bytes, tag %s, surface %s, fb-gpu %s, iconservices %s' % (
            args.payload, len(boot_args.encode()), tag, surface, fbgpu,
            'on' if 'sheng-iconservices=1' in tokens else 'off'))
        try:
            packaged_addon = subprocess.run(
                [mcopy, '-i', str(args.payload), '::/EFI/UniXNU/Payloads/kernel.macho.gpuaddon', '-'],
                capture_output=True, check=True).stdout
            adt = subprocess.run(
                [mcopy, '-i', str(args.payload), '::/EFI/UniXNU/Payloads/kernel.macho.adt', '-'],
                capture_output=True, check=True).stdout
        except subprocess.CalledProcessError as error:
            fail('%s: cannot read the GPU addon files: %s' % (args.payload, error.stderr.decode().strip()))
        if hashlib.sha256(packaged_addon).hexdigest() != hashlib.sha256(blob).hexdigest():
            fail('%s: kernel.macho.gpuaddon differs from %s/gpu-addon.z' % (args.payload, args.blob_dir))
        if b'GpuAddon' not in adt:
            fail('%s: the ADT has no GpuAddon memory-map entry; the loader has nowhere to record the range' % args.payload)
        print('PASS: %s carries this addon (%u bytes) and the ADT GpuAddon entry' % (args.payload, len(blob)))

    if args.vendor_boot:
        if not args.payload:
            fail('--vendor-boot needs --payload to know the ramdisk length')
        payload = args.payload.read_bytes()
        with args.vendor_boot.open('rb') as image:
            image.seek(0x1000)
            embedded = image.read(len(payload))
        if embedded != payload:
            fail(str(args.vendor_boot) + ': ramdisk at 0x1000 differs from ' + str(args.payload))
        print('PASS: %s carries %s at 0x1000' % (args.vendor_boot, args.payload))


if __name__ == '__main__':
    main()
