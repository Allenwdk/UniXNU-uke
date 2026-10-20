#!/usr/bin/env python3
"""Query only fixed read-only MSM capabilities from the existing Linux driver.

The layout/query IDs are pinned to the running source revision in this
repository. No GEM allocation, binding, submitqueue, command submission, fence,
SET_PARAM, timestamp register query, modeset or performance-counter call occurs.
Opening the existing render node creates/releases its ordinary per-file kernel
context; reported software capabilities do not qualify active GPU SMMU registers.
"""
import fcntl
import json
import os
from pathlib import Path
import platform
import struct

if 'xiaomi,sheng' not in Path('/sys/firmware/devicetree/base/compatible').read_bytes().decode().split('\0'):
    raise SystemExit('Refuse non-sheng capability profile')
# _IOWR('d', DRM_COMMAND_BASE(0x40) + MSM_GET_PARAM(0), 24-byte drm_msm_param)
QUERY = (3 << 30) | (24 << 16) | (ord('d') << 8) | 0x40
PARAMS = {'GPU_ID':1, 'GMEM_SIZE':2, 'CHIP_ID':3, 'MAX_FREQ':4,
          'GMEM_BASE':6, 'PRIORITIES':7, 'FAULTS':9, 'SUSPENDS':10,
          'VA_START':14, 'VA_SIZE':15, 'HIGHEST_BANK_BIT':16,
          'RAYTRACING':17, 'UBWC_SWIZZLE':18, 'MACROTILE_MODE':19,
          'UCHE_TRAP_BASE':20, 'HAS_PRR':21, 'AQE':23}
r = dict(format_version=1, existing_linux_driver=True, kernel_release=platform.release(),
         ioctl=hex(QUERY), struct_bytes=24, pipe=0x10,
         source_revision='42f3b40c702abbc37c3a04c95af008cd283aeaf3',
         device_open_mode='read only', set_param=False, gpu_test_submission=False,
         gpu_smmu_active_registers_observed=False, params={})
fd = os.open('/dev/dri/renderD128', os.O_RDONLY | os.O_CLOEXEC)
try:
    for name, param in PARAMS.items():
        buffer = bytearray(struct.pack('<IIQII', 0x10, param, 0, 0, 0))
        try:
            fcntl.ioctl(fd, QUERY, buffer, True)
            pipe, actual, value, length, pad = struct.unpack('<IIQII', buffer)
            if (pipe, actual, length, pad) != (0x10, param, 0, 0):
                raise RuntimeError('Kernel query ABI response differs')
            r['params'][name] = dict(value=value, hex=hex(value))
        except OSError as e:
            r['params'][name] = dict(errno=e.errno, error=e.strerror)
finally:
    os.close(fd)
print(json.dumps(r, indent=2))
