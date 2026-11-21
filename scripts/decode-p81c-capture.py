#!/usr/bin/env python3
"""Reassemble P81c diagnostic records from a single tagged uke boot log."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

FRAME = re.compile(r'UKE PEN FRAME (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)\s*$')
RAW = re.compile(r'UKE PEN RAW (\d+) (\d+) ([0-9a-f]+)\s*$')
HEADER_BYTES = 257
PEN_FRAME_MAX = HEADER_BYTES + 20 + 1289 * 4
CHUNK_BYTES = 64
MAX_FRAMES = 34


def checksum_extent(payload, header_offset=0):
    checksum, _, words, complement, _, length_complement = struct.unpack_from('<HHIHHI', payload, header_offset + 4)
    end = 20 + words * 4
    valid = (complement == checksum ^ 0xffff
             and length_complement == words ^ 0xffffffff
             and 20 < end <= len(payload)
             and (checksum + sum(value for value, in struct.iter_unpack('<H', payload[20:end]))) % 65536 == 0)
    return end, valid


def decode(lines):
    frames = {}
    chunks = {}
    for line in lines:
        if match := FRAME.search(line):
            index, counter, scan_type, payload_type, ns, size = map(int, match.groups())
            if index >= MAX_FRAMES or not HEADER_BYTES + 100 <= size <= PEN_FRAME_MAX:
                raise ValueError(f'invalid frame declaration {index}, size {size}')
            header = (counter, scan_type, payload_type, ns, size)
            if frames.setdefault(index, header) != header:
                raise ValueError(f'conflicting headers for frame {index}. Use one boot log')
        elif match := RAW.search(line):
            index, offset = map(int, match.groups()[:2])
            data = bytes.fromhex(match[3])
            if (index >= MAX_FRAMES or offset % CHUNK_BYTES
                    or not 0 < len(data) <= CHUNK_BYTES or offset + len(data) > PEN_FRAME_MAX):
                raise ValueError(f'invalid chunk for frame {index}, offset {offset}')
            if chunks.setdefault(index, {}).setdefault(offset, data) != data:
                raise ValueError(f'conflicting data for frame {index}, offset {offset}')

    missing = []
    complete = []
    for index in sorted(frames.keys() | chunks.keys()):
        parts = chunks.get(index, {})
        if index not in frames:
            missing.append(index)
            continue
        counter, scan_type, payload_type, ns, size = frames[index]
        offsets = range(0, size, CHUNK_BYTES)
        if any(offset not in offsets or len(data) != min(CHUNK_BYTES, size - offset)
               for offset, data in parts.items()):
            raise ValueError(f'chunk does not match frame {index} size {size}')
        if any(offset not in parts for offset in offsets):
            missing.append(index)
            continue
        raw = b''.join(parts[offset] for offset in offsets)
        payload = raw[HEADER_BYTES:]
        if (struct.unpack_from('<H', payload, 2)[0], payload[24], payload[56]) != (
                counter, scan_type, payload_type):
            raise ValueError(f'frame {index} header does not match the raw bytes')
        end, valid = checksum_extent(payload)
        record = {
            'index': index, 'counter': counter, 'scan_type': scan_type, 'payload_type': payload_type,
            'continuous_ns': ns, 'transport_bytes': len(raw), 'outer_checksum_valid': valid,
            'outer_checksum_end': end, 'outer_capture_missing_bytes': max(0, end - len(payload)),
            'sha256': hashlib.sha256(raw).hexdigest(),
        }
        if payload_type == 29 and tuple(payload[71:75]) == (12, 40, 60, 8):
            # Only the observed Uke geometry selects this trailer. The inner
            # checksum covers the four pen matrices, independently of the hand data.
            trailer = 100 + 4 * (12 * 40 + 60 * 8)
            if len(payload) >= trailer + 20:
                pen_end, pen_valid = checksum_extent(payload, trailer)
                record['pen_checksum_end'] = pen_end
                record['pen_checksum_valid'] = pen_valid and trailer <= pen_end <= trailer + 4
                record['hand_quarter'] = payload[75]
        complete.append((record, raw))
    return complete, missing


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    with args.log.open(errors='replace') as source:
        frames, missing = decode(source)
    args.output.mkdir(parents=True, exist_ok=False)
    for frame, raw in frames:
        path = args.output / f"frame-{frame['index']:02d}.bin"
        path.write_bytes(raw)
        frame['file'] = path.name
    (args.output / 'capture.json').write_text(json.dumps({
        'source_log': str(args.log), 'transport_prefix_bytes': HEADER_BYTES,
        'frames': [frame for frame, _ in frames], 'incomplete_indices': missing,
    }, indent=2) + '\n')
    print(f'{len(frames)} complete, {len(missing)} incomplete')
    return 0 if frames and not missing else 1


if __name__ == '__main__':
    raise SystemExit(main())
