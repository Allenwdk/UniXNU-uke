#!/usr/bin/env python3
"""Emit build/native-touch/NvtFirmware.c from the touch firmware kept outside Git (.cache/firmware/novatek).

The generated source embeds vendor firmware bytes, so it lives only under build/ and is never committed. With no
firmware present it emits an empty object and the touch probe stops before the download stage.
"""
import argparse
import hashlib
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# o82 is this panel (DT msm_drm.dsi_display0=qcom,mdss_dsi_o82_…); the CSOT/TM variants come from the stock ODM image
# (new-port/out/dna/odm/firmware). The TM alternative novatek_nt36532_o82_fw_tm.bin sits next to it.
DEFAULT = ROOT / '.cache/firmware/novatek/novatek_nt36532_o82_fw_csot.bin'
OUT = ROOT / 'build/native-touch'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', nargs='?', type=Path, default=DEFAULT)
    parser.add_argument('--compress', action='store_true', help='emit a zlib stream')
    args = parser.parse_args()
    path = args.firmware
    data = path.read_bytes() if path.is_file() else b''
    OUT.mkdir(parents=True, exist_ok=True)
    lines = ['/* Generated; contains vendor firmware, do not commit. */', '#include <stdint.h>']
    lines.append(f'const uint32_t native_touch_firmware_size = {len(data)};')
    image = zlib.compress(data, level=9) if args.compress and data else data
    if args.compress:
        lines.append(f'const uint32_t native_touch_firmware_packed_size = {len(image)};')
    body = ','.join(str(b) for b in image) if image else '0'
    lines.append(f'const uint8_t native_touch_firmware[{max(len(image), 1)}] = {{{body}}};')
    (OUT / 'NvtFirmware.c').write_text('\n'.join(lines) + '\n')
    print(f'touch firmware object: {len(data)} bytes ({len(image)} embedded) '
          f'sha256 {hashlib.sha256(data).hexdigest()[:16] if data else "-"}')


if __name__ == '__main__':
    main()
