#!/usr/bin/env python3
"""Read existing sheng Linux facts; never open device nodes or write sysfs/MMIO.

Run on the already-running Linux system over an authorized SSH session. No
credentials or network destination belong in this collector or its report.
Root optionally permits the existing DRM state file; it does not load code,
reboot, change modes, submit GPU work or qualify Darwin/GPU active registers.
"""
import argparse
import glob
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--debugfs', action='store_true', help='Read existing DRM state only; requires current permissions')
a = p.parse_args()
DT = Path('/sys/firmware/devicetree/base')


def data(path, bound=65536):
    with Path(path).open('rb') as f:
        raw = f.read(bound+1)
    if len(raw) > bound:
        raise ValueError('Read exceeds explicit bound')
    return raw


def strings(raw):
    return [v.decode('utf-8') for v in raw.split(b'\0') if v]


def cells(raw):
    if len(raw) % 4:
        raise ValueError('Device-tree cell extent')
    return list(struct.unpack('>' + str(len(raw)//4) + 'I', raw))


r = dict(format_version=1, existing_linux_only=True, kernel_release=platform.release(),
         machine=platform.machine(), linux_userspace_page_bytes=os.sysconf('SC_PAGE_SIZE'),
         model=strings(data(DT/'model')), compatible=strings(data(DT/'compatible')),
         source_of_chip_identity='running device tree, not a GPU register read',
         cpu_tcr_ttbr_sctlr_observed=False, gpu_smmu_active_registers_observed=False,
         darwin_hardware_boot_observed=False, target_metal_or_ui_observed=False,
         device_writes=False, gpu_test_submission=False)
if r['compatible'] != ['xiaomi,sheng', 'qcom,sm8550']:
    raise SystemExit('Refuse non-sheng collection profile')
nodes = ['soc@0/gpu@3d00000', 'soc@0/gmu@3d6a000', 'soc@0/iommu@3da0000',
         'soc@0/display-subsystem@ae00000/dsi@ae94000/panel@0']
r['device_tree'] = {}
for name in nodes:
    props = {}
    for prop in ['compatible','reg','reg-names','iommus','phandle','#iommu-cells','dma-coherent','status']:
        try:
            raw = data(DT/name/prop)
        except FileNotFoundError:
            continue
        if prop in ('compatible','reg-names','status'):
            value = strings(raw)
        elif prop == 'dma-coherent':
            value = raw == b''
        else:
            value = cells(raw)
        props[prop] = value
    r['device_tree'][name] = props
r['display_connectors'] = {}
for name in ('card0-DSI-1','card0-DP-1'):
    r['display_connectors'][name] = {}
    for prop in ('status','enabled','modes'):
        try:
            r['display_connectors'][name][prop] = data('/sys/class/drm/'+name+'/'+prop).decode().splitlines()
        except OSError as e:
            r['display_connectors'][name][prop] = dict(error=e.strerror)
r['firmware'] = {}
for name in ('a740_sqe.fw','gmu_gen70200.bin','sm8550/sheng/a740_zap.mbn'):
    try:
        raw = data(Path('/lib/firmware/qcom')/name, 1024*1024)
        r['firmware'][name] = dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                                   file_read_only=True, image_transferred=False)
    except OSError as e:
        r['firmware'][name] = dict(error=e.strerror)
r['iommu_groups'] = {}
for name in ('3d00000.gpu','3d6a000.gmu'):
    try:
        group = (Path('/sys/bus/platform/devices')/name/'iommu_group').resolve(strict=True)
        r['iommu_groups'][name] = dict(group=int(group.name), devices=sorted(v.name for v in (group/'devices').iterdir()))
    except OSError as e:
        r['iommu_groups'][name] = dict(error=e.strerror)
if a.debugfs:
    try:
        raw = data('/sys/kernel/debug/dri/0/state')
        text = raw.decode()
        # Publish modes and resource selection, never framebuffer contents,
        # process/client lists or arbitrary GPU/DPU register dumps.
        r['existing_drm_state'] = dict(sha256=hashlib.sha256(raw).hexdigest(),
            relevant_lines=[v.strip() for v in text.splitlines() if re.match(
                r'\s*(?:plane\[|crtc\[|connector\[|active=|enable=|mode:|format=|modifier=|size=|pitch\[|lm\[|ctl\[|dspp\[|dsc=)', v)],
            screenshot_captured=False, modes_changed=False)
    except OSError as e:
        r['existing_drm_state'] = dict(error=e.strerror)
print(json.dumps(r, indent=2))
