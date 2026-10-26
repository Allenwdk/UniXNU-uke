#!/usr/bin/env python3
"""Extract the UKELLOG1 ring from a userdata dump.

The kernel's UkeLastLog writer appends 16 KiB records to the last 32 MiB of the
userdata partition. Each record starts with

    [ 0] magic "UKELLOG1"
    [ 8] sequence number, little endian u64
    [16] payload length, u32
    [20] payload crc32, u32
    [32] payload: the newest bytes of the kernel message buffer

Usage:
    # On the device (recovery / TWRP adb shell), dump the window:
    dd if=/dev/block/by-name/userdata bs=4096 \
       skip=$((LAST_LBA + 1 - 8192)) count=8192 of=/tmp/ukelog.bin
    adb pull /tmp/ukelog.bin

    # Then here:
    python3 tools/decode-ukelog.py ukelog.bin -o boot.log

The decoder keeps every record whose magic and CRC are valid, sorts them by
sequence number, and writes the newest contiguous run to the output file.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

RECORD = 16 * 1024
MAGIC = b'UKELLOG1'


def records(data):
    found = {}
    for off in range(0, len(data) - RECORD + 1, RECORD):
        chunk = data[off:off + RECORD]
        if chunk[:8] != MAGIC:
            continue
        seq, length, crc = struct.unpack_from('<QII', chunk, 8)
        if length == 0 or length > RECORD - 32:
            continue
        payload = chunk[32:32 + length]
        if zlib.crc32(payload) & 0xffffffff != crc:
            continue
        found[seq] = payload
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', type=Path, help='the 32 MiB userdata window, raw')
    ap.add_argument('-o', '--output', type=Path, default=Path('ukelog.txt'))
    ap.add_argument('--all', action='store_true',
                    help='write every valid record in sequence order instead of '
                         'only the newest contiguous run')
    args = ap.parse_args()

    data = args.image.read_bytes()
    if len(data) < RECORD:
        sys.exit(f'{args.image}: {len(data)} bytes, too short for one record')

    found = records(data)
    if not found:
        sys.exit('no valid UKELLOG1 record; dump the right window?')

    seqs = sorted(found)
    if args.all:
        chosen = seqs
    else:
        # The writer wraps, so the newest run is the one ending at the highest
        # sequence number and extending backwards without a gap.
        chosen = [seqs[-1]]
        for seq in reversed(seqs[:-1]):
            if seq == chosen[0] - 1:
                chosen.insert(0, seq)
            else:
                break

    args.output.write_bytes(b''.join(found[s] for s in chosen))
    print(f'{len(found)} valid records, sequence {seqs[0]}..{seqs[-1]}')
    print(f'wrote {len(chosen)} records ({args.output.stat().st_size} bytes) to {args.output}')


if __name__ == '__main__':
    main()
