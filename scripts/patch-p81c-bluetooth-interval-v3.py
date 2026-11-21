# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Keep the P81c connection-parameter request from entering 24A446 reject paths."""

import runpy
import struct
from pathlib import Path

BASE = 0x100000000
CHECK_ONE = 0x100810c1c
CHECK_TWO = 0x100810c40
NOP = 0xD503201F


def patch(data, compiler='clang'):
    active = runpy.run_path(str(Path(__file__).with_name('patch-p81c-bluetooth-interval.py')))
    result = bytearray(active['patch'](data, compiler))
    for address, expected in ((CHECK_ONE, 0x34000400), (CHECK_TWO, 0x340002E0)):
        offset = address - BASE
        if struct.unpack_from('<I', result, offset)[0] != expected:
            raise ValueError(f'P81c connection policy site differs at {address:#x}')
        struct.pack_into('<I', result, offset, NOP)
    return bytes(result)
