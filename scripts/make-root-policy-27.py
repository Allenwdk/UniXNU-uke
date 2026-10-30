#!/usr/bin/env python3
"""Sign and fully verify the original 24A446 OS image: the root policy of source storage.

macOS attaches the image with hdiutil/diskutil and builds the verification library as a dylib; Linux decrypts the
AEA member with blacktop's ipsw, reads the raw disk with the core's udif.py, and builds the same library as a
shared object against the system OpenSSL. The kernel only carries the public anchor (trusted-key.bin); the private
key never leaves the machine.
"""
import argparse
import contextlib
import ctypes
import hashlib
import importlib.util
import json
import plistlib
import shutil
import stat
import subprocess
import sys
import tomllib
import uuid
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image', type=Path, required=True, help='the OS disk image extracted from the IPSW (.dmg or .dmg.aea)')
parser.add_argument('--profile', type=Path, required=True, help='profiles/os/<profile>/profile.toml (core checkout)')
parser.add_argument('--verified-root', type=Path, required=True, help='kits/VerifiedRoot checkout')
parser.add_argument('--output', type=Path, required=True, help='policy directory (manifest.bin, trusted-key.bin, ...)')
parser.add_argument('--host', type=Path, help='keys/ and the verification library (default: output/host)')
parser.add_argument('--core', type=Path, default=Path(__file__).resolve().parents[2] / 'UniXNU-build',
                    help='UniXNU core checkout; Linux imports scripts/unixnu/udif.py from it')
parser.add_argument('--ipsw-tool', default=None, help='blacktop ipsw binary for AEA decryption (default: ipsw on PATH)')
args = parser.parse_args()
profile = tomllib.loads(args.profile.read_text())
root_profile = profile['root']
expected_uuid = uuid.UUID(root_profile['os_volume_uuid'])
expected_digest = root_profile['os_image_sha256']
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
host = (args.host or out / 'host').resolve()
keys = host / 'keys'
keys.mkdir(parents=True, exist_ok=True)


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


if not (keys / 'owner.pem').is_file():
    subprocess.run(['openssl', 'genrsa', '-out', str(keys / 'owner.pem'), '3072'], check=True)
    subprocess.run(['openssl', 'rsa', '-in', str(keys / 'owner.pem'), '-pubout', '-out', str(keys / 'owner-public.pem')],
                   check=True, capture_output=True)
library = host / ('verified-root.dylib' if sys.platform == 'darwin' else 'verified-root.so')
if not library.is_file():
    include = args.verified_root / 'include'
    sources = [args.verified_root / 'src/verified_root.c', args.verified_root / 'tests/openssl_provider.c']
    if sys.platform == 'darwin':
        openssl = subprocess.check_output(['brew', '--prefix', 'openssl@3'], text=True).strip()
        command = ['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-dynamiclib',
                   '-I', str(include), '-I', f'{openssl}/include', *map(str, sources),
                   '-L', f'{openssl}/lib', '-lcrypto', '-o', str(library)]
    else:
        command = [shutil.which('clang') or 'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                   '-I', str(include), *map(str, sources), '-lcrypto', '-o', str(library)]
    subprocess.run(command, check=True)


def decrypt_aea(path):
    """AEA member to a plain disk image beside it, through blacktop's ipsw (Linux and macOS both have it)."""
    ipsw = args.ipsw_tool or shutil.which('ipsw')
    if not ipsw:
        raise SystemExit('AEA decryption needs blacktop\'s ipsw (https://github.com/blacktop/ipsw); pass --ipsw-tool')
    result = subprocess.run([ipsw, 'fw', 'aea', '-k', str(path)], capture_output=True, text=True)
    keys_out = [line.strip() for line in result.stdout.splitlines() if line.strip().startswith('base64:')]
    if result.returncode or len(keys_out) != 1:
        raise SystemExit(f'could not read the AEA key of {path.name}: {result.stderr.strip()[-300:]}')
    plain = path.with_suffix('')
    if plain.is_file() and plain.stat().st_size:
        return plain
    before = {p.name for p in path.parent.iterdir()}
    result = subprocess.run([ipsw, 'fw', 'aea', '-b', keys_out[0], '-o', str(path.parent), str(path)],
                            capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f'aea decrypt of {path.name} failed: {result.stderr.strip()[-300:]}')
    after = [p for p in path.parent.iterdir() if p.name not in before and p.is_file()]
    if len(after) != 1:
        raise SystemExit(f'aea decrypt of {path.name} wrote {[p.name for p in after]}, expected one image')
    return after[0]


class BlocksReader:
    """A sequential reader over a raw disk image; manifest.make only reads forward."""

    def __init__(self, blocks):
        self.blocks, self.buffer = iter(blocks), bytearray()

    def read(self, size):
        while len(self.buffer) < size:
            try:
                self.buffer += next(self.blocks)
            except StopIteration:
                break
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    def close(self):
        self.blocks = iter(())


source = args.image.resolve()
compressed_digest = sha256_file(source)
plain = decrypt_aea(source) if source.suffix == '.aea' else source
image_bytes = None
read_raw = None
detach = None
try:
    if sys.platform == 'darwin':
        if plain.suffix == '.aea' or not plain.is_file():
            raise SystemExit('macOS needs a plain .dmg (the AEA member is decrypted through ipsw first)')
        attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-nomount', '-readonly', '-noverify',
                                                           '-noautofsck', '-plist', str(plain)]))
        entities = attached['system-entities']
        whole = [e['dev-entry'] for e in entities if e.get('content-hint') == 'GUID_partition_scheme']
        if len(whole) != 1:
            raise SystemExit('Require one whole virtual media mapping')
        detach = whole[0]
        info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', whole[0]]))
        if info.get('BusProtocol') != 'Disk Image' or info.get('Writable') is not False or info.get('DeviceNode') != whole[0]:
            raise SystemExit('Only the actual read-only Disk Image is accepted')
        image_bytes = info['TotalSize']
        disk = Path('/dev/r' + Path(whole[0]).name)
        if not stat.S_ISCHR(disk.stat().st_mode):
            raise SystemExit('Require the verified virtual-media raw character device')
        read_raw = lambda: disk.open('rb', buffering=1 << 20)
        host_note = 'Disk Image'
    else:
        sys.path.insert(0, str(args.core.resolve() / 'scripts'))
        from unixnu import udif
        image = udif.Image(plain)
        image_bytes = image.size
        read_raw = lambda: BlocksReader(image.blocks())
        host_note = 'Disk Image (UDIF via udif.py)'

    spec = importlib.util.spec_from_file_location('verified_manifest', args.verified_root / 'tools/manifest.py')
    manifest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manifest)
    with contextlib.closing(read_raw()) as stream:
        record = manifest.make(stream, image_bytes, keys / 'owner.pem', keys / 'owner-public.pem',
                               expected_uuid, 1, out)
finally:
    if detach:
        subprocess.run(['hdiutil', 'detach', detach], capture_output=True)


class View(ctypes.Structure):
    _fields_ = [('hashes', ctypes.c_void_p), ('image_bytes', ctypes.c_uint64), ('block_count', ctypes.c_uint64),
                ('generation', ctypes.c_uint64), ('volume_uuid', ctypes.c_uint8 * 16),
                ('image_sha256', ctypes.c_uint8 * 32)]


native = ctypes.CDLL(str(library))
native.vr_host_open.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_uint64,
                                ctypes.POINTER(View)]
native.vr_host_block.argtypes = [ctypes.POINTER(View), ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
encoded = ctypes.create_string_buffer((out / 'manifest.bin').read_bytes())
anchor = ctypes.create_string_buffer((out / 'trusted-key.bin').read_bytes())
view = View()
if native.vr_host_open(encoded, len(encoded) - 1, anchor, 1, ctypes.byref(view)) or \
        bytes(view.volume_uuid) != expected_uuid.bytes:
    raise SystemExit('Real C signature/volume authentication failed')
complete = hashlib.sha256()
indices = [0, 1, view.block_count // 2, view.block_count - 1]
samples, lengths = [], []
with contextlib.closing(read_raw()) as stream:
    for index in range(view.block_count):
        block = stream.read(min(4096, view.image_bytes - index * 4096))
        if native.vr_host_block(ctypes.byref(view), index, block, len(block)) != 0:
            raise SystemExit('Actual C block verification failed at ' + str(index))
        complete.update(block)
        if index in indices:
            samples.append(block.ljust(4096, b'\0'))
            lengths.append(len(block))
    if stream.read(1):
        raise SystemExit('Virtual media exceeded its authenticated exact bounds')
if complete.digest() != bytes(view.image_sha256) or complete.hexdigest() != expected_digest:
    raise SystemExit('Actual complete original OS digest differs from the profile')
(out / 'original-samples.bin').write_bytes(b''.join(samples))
report = dict(image_bytes=view.image_bytes, blocks_verified=view.block_count, image_sha256=complete.hexdigest(),
              volume_uuid=str(expected_uuid).upper(), generation=view.generation,
              source_compressed_sha256=compressed_digest, source_format='UDIF',
              host_bus_protocol=host_note, host_media_writable=False,
              trusted_key_sha256=record['trusted_key_sha256'], actual_sample_indices=indices,
              actual_sample_lengths=lengths, real_c_signature_and_block_verification=True,
              original_image_modified=False, host_only=True, boot_root_authenticated=False,
              qemu_tested=False, hardware_tested=False, os_profile=profile['os']['name'])
(out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'PASS: {profile["os"]["name"]} OS policy, {view.block_count} blocks, uuid {expected_uuid}')
