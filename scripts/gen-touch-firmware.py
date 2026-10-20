#!/usr/bin/env python3
"""Emit build/native-touch/NvtFirmware.c from the touch firmware kept outside Git (.cache/firmware/novatek).

The generated source embeds vendor firmware bytes, so it lives only under build/ and is never committed. With no
firmware present it emits an empty object and the touch probe stops before the download stage.
"""
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = ROOT / '.cache/firmware/novatek/novatek_nt36532_n81a_fw_csot.bin'
OUT = ROOT / 'build/native-touch'


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    data = path.read_bytes() if path.is_file() else b''
    OUT.mkdir(parents=True, exist_ok=True)
    lines = ['/* Generated; contains vendor firmware, do not commit. */', '#include <stdint.h>']
    lines.append(f'const uint32_t native_touch_firmware_size = {len(data)};')
    body = ','.join(str(b) for b in data) if data else '0'
    lines.append(f'const uint8_t native_touch_firmware[{max(len(data), 1)}] = {{{body}}};')
    (OUT / 'NvtFirmware.c').write_text('\n'.join(lines) + '\n')
    print(f'touch firmware object: {len(data)} bytes sha256 {hashlib.sha256(data).hexdigest()[:16] if data else "-"}')


if __name__ == '__main__':
    main()
