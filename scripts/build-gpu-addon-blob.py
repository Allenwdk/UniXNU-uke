#!/usr/bin/env python3
"""Build the GPU addon the boot payload hands to the kernel.

The addon is packed by the core packer (scripts/make-gpu-addon.py, format
NGPUADD2: one zlib stream per file) from the owner-built NativeAdrenoMetal
bundle, its publish/client markers and the probe daemon (build/
adreno-metal-plugin in the UniXNU core checkout). The payload carries it as
Payloads/kernel.macho.gpuaddon; the DarwinBoot loader places it after the
trust caches and records it as /chosen/memory-map/GpuAddon, and the kernel
writes every file whose content or mode differs into the mounted Data volume
before the addon bindfs mount (sheng-gpu-addon=1).

  build-gpu-addon-blob.py [--plugin-dir DIR] [--output-dir DIR] [--clients NAMES]
                          [--no-publish]

This script builds the plugin (and refreshes the owner trust module that
admits its CDHashes) when it is not on disk yet, so a kernel build from a
clean tree produces a matching blob + trust cache. The payload step picks the
blob up through board.toml [payload] gpu_addon."""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b'NGPUADD2'
ENTRY = struct.Struct('<IIQQ')       # path bytes (with NUL), mode, size, compressed size
CLIENTS = 'iconservicesagent,PosterBoard,WidgetRenderer_Default,CollectionsPoster,AegirPoster,KaleidoscopePoster'
OWNER_TRUST = Path('/home/allenwdk/UniXNU/uke-port/data/owner-trust-cache.bin')


def core_root():
    env = os.environ.get('UNIXNU_ROOT')
    if env:
        return Path(env).resolve()
    if (ROOT / 'scripts/make-gpu-addon.py').is_file():
        return ROOT
    return Path('/home/allenwdk/UniXNU/UniXNU-format2')


def plugin_dir(explicit):
    if explicit:
        return explicit.resolve()
    env = os.environ.get('UKE_GPU_PLUGIN')
    if env:
        return Path(env).resolve()
    candidate = core_root() / 'build/adreno-metal-plugin'
    if (candidate / 'NativeAdrenoMetal.bundle/NativeAdrenoMetal').is_file():
        return candidate
    return Path('/home/allenwdk/UniXNU/UniXNU/build/adreno-metal-plugin')


def ensure_inputs(plugin: Path):
    """Build the owner plugin when it is not there yet and always refresh the
    owner trust module from the same plugin build the blob packs."""
    root = plugin.parent.parent        # <checkout>/build/adreno-metal-plugin
    build = root / 'scripts/build-adreno-metal-plugin.py'
    if not (plugin / 'NativeAdrenoMetal.bundle/NativeAdrenoMetal').is_file():
        if not build.is_file():
            sys.exit('missing plugin input and no build script at ' + str(build))
        subprocess.run(['python3', str(build)], check=True)
    trust = root / 'scripts/make-owner-trust-bundle.py'
    if trust.is_file():
        subprocess.run(['python3', str(trust)], check=True)
        owner = Path(os.environ.get('UKE_OWNER_TRUST', str(OWNER_TRUST)))
        shutil.copyfile(root / 'build/adreno-metal-plugin/owner-trust.bundle', owner)


def pack(plugin: Path, output: Path, clients: str, publish: bool, wallpaper: Path | None, force_generic: bool):
    root = core_root()
    packer = root / 'scripts/make-gpu-addon.py'
    if not packer.is_file():
        packer = ROOT / 'scripts/make-gpu-addon.py'
    if not packer.is_file():
        sys.exit('missing the core addon packer (scripts/make-gpu-addon.py)')
    output.unlink(missing_ok=True)
    command = ['python3', str(packer), '--plugin-dir', str(plugin), '--clients', clients,
               '--daemon-plist', 'org.blu-sharky.adrenoprobed.plist']
    if wallpaper:
        command += ['--wallpaper', str(wallpaper)]
    if force_generic:
        command.append('--force-generic')
    if publish:
        command.append('--publish')
    command.append(str(output))
    subprocess.run(command, check=True)


def describe(blob: bytes):
    """Parse the NGPUADD2 stream the way kern_exec.c validates it and return
    the entry list for gpu-addon.json."""
    if len(blob) < 24 or blob[:8] != MAGIC:
        sys.exit('the packer did not produce an NGPUADD2 stream')
    count, reserved = struct.unpack_from('<II', blob, 8)
    total = struct.unpack_from('<Q', blob, 16)[0]
    if total != len(blob) or not count or count > 4096:
        sys.exit('the addon header counts do not match the file')
    at, entries = 24, []
    for index in range(count):
        name_bytes, mode, size, packed = ENTRY.unpack_from(blob, at)
        at += ENTRY.size
        name_padded = (name_bytes + 7) & ~7
        size_padded = (packed + 7) & ~7
        name = blob[at:at + name_bytes].split(b'\0', 1)[0].decode()
        stream = blob[at + name_padded:at + name_padded + packed]
        data = zlib.decompress(stream)
        if len(data) != size:
            sys.exit(name + ': zlib stream length differs from the header')
        entries.append({'path': name, 'mode': mode, 'bytes': size, 'sha256': hashlib.sha256(data).hexdigest()})
        at += name_padded + size_padded
    if at != len(blob):
        sys.exit('trailing bytes after the addon entries')
    return count, entries


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--plugin-dir', type=Path, default=None)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build/uke-gpu-addon')
    parser.add_argument('--clients', default=CLIENTS)
    parser.add_argument('--wallpaper', type=Path, default=None,
                        help='wallpaper overlay directory (scripts/make-wallpaper-overlay.py); default: uke-port/data/wallpaper-overlay when present')
    parser.add_argument('--no-publish', action='store_true',
                        help='probe image: omit the .publish marker so the plugin runs its shared user test and returns nil (software rendering kept)')
    parser.add_argument('--force-generic', action='store_true',
                        help='add NativeAdrenoMetal.generic: the plugin makes QuartzCore take its generic-shader path')
    args = parser.parse_args()
    plugin = plugin_dir(args.plugin_dir)
    wallpaper = args.wallpaper
    if wallpaper is None:
        default = Path('/home/allenwdk/UniXNU/uke-port/data/wallpaper-overlay')
        wallpaper = default if default.is_dir() else None
    ensure_inputs(plugin)
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    blob_path = out / 'gpu-addon.z'
    pack(plugin, blob_path, args.clients, not args.no_publish, wallpaper, args.force_generic)
    blob = blob_path.read_bytes()
    count, entries = describe(blob)
    report = {
        'blob': str(blob_path), 'bytes': len(blob), 'entries': entries,
        'blob_sha256': hashlib.sha256(blob).hexdigest(),
        'format': 'NGPUADD2/2 (per-file zlib; scripts/make-gpu-addon.py)',
    }
    (out / 'gpu-addon.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: GPU addon blob %d bytes; %d entries%s' % (len(blob), count,
                                                           ' (probe, no publish marker)' if args.no_publish else ''))


if __name__ == '__main__':
    main()
