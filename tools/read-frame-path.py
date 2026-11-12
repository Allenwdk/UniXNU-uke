#!/usr/bin/env python3
"""Read Uke's two completed frame-path exports from TWRP or a saved export region."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

BLOCK = 4096
EXPORT_BLOCKS = 65536
LOG_BLOCKS = 8192
HEADER = struct.Struct('<IIIIQ')


def unpack(blob, destination):
    if len(blob) < 33 or blob[:8] != b'SHEXR001':
        raise ValueError('Incomplete frame-path header')
    size, captured = struct.unpack_from('<II', blob, 8)
    if size + 16 != len(blob) or blob[16:24] != b'M2VCACH1':
        raise ValueError('Invalid frame-path container length')
    root_size = struct.unpack_from('<I', blob, 24)[0]
    at = 28
    if blob[at:at+root_size] != b'/':
        raise ValueError('Unexpected frame-path root')
    at += root_size
    count = struct.unpack_from('<I', blob, at)[0]
    at += 4
    if not 1 <= count <= 256:
        raise ValueError('Invalid exported file count')
    records = []
    for _ in range(count):
        if at + HEADER.size > len(blob):
            raise ValueError('Incomplete exported file header')
        length, mode, uid, gid, size = HEADER.unpack_from(blob, at)
        at += HEADER.size
        if not 1 <= length <= 4096 or at + length + size > len(blob):
            raise ValueError('Invalid exported file range')
        name = blob[at:at+length].decode()
        at += length
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts or not relative.parts:
            raise ValueError('Exported path escapes the output directory')
        records.append((relative, mode, uid, gid, blob[at:at+size]))
        at += size
    if at != len(blob):
        raise ValueError('Unexpected bytes after exported files')
    if destination.exists():
        raise ValueError(f'Output already exists: {destination}')
    files = destination/'files'
    files.mkdir(parents=True)
    manifest = []
    for name, mode, uid, gid, body in records:
        target = files/name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            raise ValueError(f'Duplicate exported path: {name}')
        target.write_bytes(body)
        manifest.append(dict(path=str(name), bytes=len(body), sha256=hashlib.sha256(body).hexdigest(),
                             mode=mode, uid=uid, gid=gid))
    (destination/'export.bin').write_bytes(blob)
    (destination/'manifest.json').write_text(json.dumps(dict(captured_uptime_s=captured,
        sha256=hashlib.sha256(blob).hexdigest(), files=manifest), indent=2)+'\n')
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--serial', help='Uke in TWRP')
    source.add_argument('--image', type=Path, help='saved 256 MiB export region')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.image:
        if args.image.stat().st_size != EXPORT_BLOCKS*BLOCK:
            parser.error('Expected a complete 256 MiB export region')
        stream = args.image.open('rb')

        def read(first, count):
            stream.seek(first*BLOCK)
            data = stream.read(count*BLOCK)
            if len(data) != count*BLOCK:
                raise ValueError('Short read of saved region')
            return data
    else:
        adb = ['adb', '-s', args.serial]

        def shell(*words):
            return subprocess.check_output(adb+['shell', *words], text=True, timeout=15).strip()

        if shell('getprop', 'ro.product.device') != 'uke' or not shell('getprop', 'ro.twrp.version'):
            parser.error('The selected device must be Uke in TWRP')
        sectors = int(shell('blockdev', '--getsz', '/dev/block/by-name/userdata'))
        if sectors != 471908312:
            parser.error('Unexpected Uke userdata geometry')
        base = sectors//8-LOG_BLOCKS-EXPORT_BLOCKS

        def read(first, count):
            command = adb+['exec-out', 'dd', 'if=/dev/block/by-name/userdata', 'bs=4096',
                           f'skip={base+first}', f'count={count}', 'status=none']
            result = subprocess.run(command, capture_output=True, timeout=60, check=True)
            if len(result.stdout) != count*BLOCK:
                raise ValueError('Short ADB read')
            return result.stdout

    args.out.mkdir(parents=True, exist_ok=True)
    found = []
    for first in (0, EXPORT_BLOCKS//2):
        head = read(first, 1)
        if head[:8] != b'SHEXR001':
            continue
        size, captured = struct.unpack_from('<II', head, 8)
        if not 17 <= size <= EXPORT_BLOCKS//2*BLOCK-16:
            raise ValueError('Export exceeds its half')
        total = size+16
        count = (total+BLOCK-1)//BLOCK
        data = bytearray()
        for offset in range(0, count, 1024):
            data.extend(read(first+offset, min(1024, count-offset)))
        destination = args.out/f'capture-{captured:06d}-half-{first}'
        files = unpack(bytes(data[:total]), destination)
        scripts = Path(__file__).resolve().parents[1]/'scripts'
        gpu = files/'gpu.trace.bin'
        if gpu.exists():
            subprocess.run([sys.executable, scripts/'decode-gpu-trace.py', files,
                            '--out', destination/'gpu'], check=True)
        latency = files/'kernel.latency.bin'
        if latency.exists():
            frames = sorted(files.rglob('frames*.trace'))
            for index, frame in enumerate(frames or [None]):
                command = [sys.executable, scripts/'decode-sheng-latency.py', latency,
                           '--out', destination/f'latency-{index}.jsonl']
                if frame:
                    command += ['--frames', frame]
                if (files/'kernel.touch.bin').exists():
                    command += ['--touch', files/'kernel.touch.bin']
                subprocess.run(command, check=True)
        found.append(dict(first_block=first, captured_uptime_s=captured, directory=str(destination)))
    if args.image:
        stream.close()
    if not found:
        parser.error('No completed frame-path export in either half')
    (args.out/'captures.json').write_text(json.dumps(found, indent=2)+'\n')
    print(json.dumps(found))


if __name__ == '__main__':
    main()
