#!/usr/bin/env python3
"""Check the assembled FD's vendor bytes, DEPEX and APRIORI references."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
fd = ROOT / 'uefi-sheng/Build/shengPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd'
dump = fd.with_name(fd.name + '.dump')
digest = hashlib.sha256(fd.read_bytes()).hexdigest()
stamp = fd.with_name(fd.name + '.dump.sha256')
if not dump.exists() or not stamp.exists() or stamp.read_text().strip() != digest:
    if dump.exists():
        shutil.rmtree(dump)
    with (ROOT / '.cache/sheng-candidate-fv.log').open('w') as log:
        subprocess.run([str(ROOT / 'build/tools/uefiextract/uefiextract'), str(fd), 'all'],
                       check=True, stdout=log, stderr=log)
    stamp.write_text(digest + '\n')
manifest = json.loads((ROOT / '.cache/firmware/sheng/uefi-binaries/manifest.json').read_text())
stock = {entry['inf']: entry for entry in manifest['modules']}
errors = []
files = {}
volumes = []
apriori = []


def fields(path):
    result = {}
    for line in path.read_text().splitlines():
        if ': ' in line:
            key, value = line.split(': ', 1)
            result.setdefault(key, value)
    return result


for info in dump.rglob('info.txt'):
    metadata = fields(info)
    if metadata.get('Type') == 'Volume':
        volumes.append(metadata)
    if metadata.get('Type') != 'File' or not metadata.get('File GUID'):
        continue
    guid = metadata['File GUID'].lower()
    if guid in files:
        errors.append(f'Duplicate FFS GUID: {guid}')
    sections = {}
    for child in info.parent.rglob('info.txt'):
        section = fields(child)
        body = child.with_name('body.bin')
        if section.get('Type') == 'Section' and body.exists():
            sections[section['Subtype']] = body.read_bytes()
    files[guid] = (metadata, sections)
    if 'invalid' in metadata.get('Header checksum', '') or 'invalid' in metadata.get('Data checksum', ''):
        errors.append(f'Invalid FFS checksum: {guid}')
    if guid == 'fc510ee7-ffdc-11d4-bd41-0080c73c8881':
        body = sections['Raw']
        if len(body) % 16:
            errors.append('Invalid APRIORI byte count')
        else:
            apriori = [str(uuid.UUID(bytes_le=body[index:index + 16])) for index in range(0, len(body), 16)]
selected = []
include = ROOT / 'uefi-sheng/Platforms/Xiaomi/shengPkg/Include/DXE.inc'


def active_lines(path):
    """Lines of an FDF include that survive the board switches in sheng.dsc."""
    dsc = (ROOT / 'uefi-sheng/Platforms/Xiaomi/shengPkg/sheng.dsc').read_text()
    values = {name: int(value) for name, value in re.findall(r'^\s*(USE_CUSTOM_DISPLAY_DRIVER|SHENG_VENDOR_HW)\s*=\s*(\d+)', dsc, re.MULTILINE)}
    keep = [True]
    for line in path.read_text().splitlines():
        word = line.strip()
        if word.startswith('!if'):
            match = re.fullmatch(r'!if \$\((\w+)\) == (\d+)', word)
            if not match or match.group(1) not in values:
                raise SystemExit(f'Unsupported conditional in {path.name}: {word}')
            keep.append(keep[-1] and values[match.group(1)] == int(match.group(2)))
        elif word == '!else':
            keep[-1] = keep[-2] and not keep[-1]
        elif word == '!endif':
            keep.pop()
        elif keep[-1]:
            yield line


for path in re.findall(r'^\s*INF\s+Binaries/sheng/([^\s}]+)', '\n'.join(active_lines(include)), re.MULTILINE):
    entry = stock[path]
    guid = entry['guid'].lower()
    selected.append(path)
    if guid not in files:
        errors.append(f'Missing vendor FFS: {path}')
        continue
    _, sections = files[guid]
    code = sections.get('PE32 image', sections.get('TE image', b''))
    if hashlib.sha256(code).hexdigest() != entry['sha256']:
        errors.append(f'Changed vendor executable: {path}')
    depex = sections.get('DXE dependency')
    if (hashlib.sha256(depex).hexdigest() if depex is not None else None) != entry['depex_sha256']:
        errors.append(f'Changed vendor DEPEX: {path}')
dangling = [guid for guid in apriori if guid not in files]
errors.extend(f'APRIORI refers to missing FFS: {guid}' for guid in dangling)
if not apriori:
    errors.append('No APRIORI list found')
if fd.stat().st_size != 0x300000:
    errors.append('FD does not match configured size')
for guid in ('9b665204-a133-47a2-9396-9e2b370a4e25', '29b950cf-586d-49ea-9d71-f88645ccdaf6'):
    if guid not in files:
        errors.append(f'Missing CrabApple component: {guid}')
report = {'fd_sha256': hashlib.sha256(fd.read_bytes()).hexdigest(), 'fd_bytes': fd.stat().st_size,
          'ffs_files': len(files), 'volumes': len(volumes), 'vendor_modules_checked': len(selected),
          'apriori_guids': len(apriori), 'dangling_apriori_guids': dangling,
          'errors': errors, 'hardware_boot_demonstrated': False}
path = ROOT / 'build/sheng/fv-check.json'
path.write_text(json.dumps(report, indent=2) + '\n')
print(f'{len(files)} FFS files; {len(selected)} original vendor modules checked; {len(errors)} errors')
print(f'{len(dangling)} APRIORI references have no corresponding FFS file')
for error in errors:
    print(error)
raise SystemExit(bool(errors))
