#!/usr/bin/env python3
"""Extract genuine sheng binary modules and generate local EDK II INF wrappers."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
firmware = ROOT / '.cache/firmware/sheng'
inputs = json.loads((firmware / 'porting-inputs.json').read_text())
record = next(entry for entry in inputs['selected'] if entry['file'] == 'uefi.elf')
image = firmware / 'porting-inputs/uefi.elf'
if hashlib.sha256(image.read_bytes()).hexdigest() != record['sha256']:
    raise SystemExit('UEFI input differs from verified archive extraction')
dump = image.with_name(image.name + '.dump')
if not dump.exists():
    subprocess.run([str(ROOT / 'build/tools/uefiextract/uefiextract'), str(image), 'all'], check=True)
output = firmware / 'uefi-binaries'
output.mkdir(exist_ok=True)
modules = []
raw_files = []
types = {'DXE driver': 'DXE_DRIVER', 'UEFI application': 'UEFI_APPLICATION',
         'DXE core': 'DXE_CORE', 'SEC core': 'SEC', 'PEI core': 'PEI_CORE',
         'PEI module': 'PEIM', 'Combined SMM/DXE': 'DXE_SMM_DRIVER'}


def fields(path):
    result = {}
    for line in path.read_text().splitlines():
        if ': ' in line:
            key, value = line.split(': ', 1)
            result.setdefault(key, value)
    return result


def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() != data:
        raise ValueError(f'Conflicting extracted output: {path.relative_to(output)}')
    path.write_bytes(data)


for info in dump.rglob('info.txt'):
    entry = fields(info)
    if entry.get('Type') != 'File':
        continue
    guid = entry.get('File GUID')
    if not guid:
        continue
    if entry.get('Subtype') not in types and entry.get('Subtype') != 'Freeform':
        continue
    ui = entry.get('Text', guid)
    sections = {}
    for child in info.parent.rglob('info.txt'):
        section = fields(child)
        if section.get('Type') != 'Section':
            continue
        kind = section.get('Subtype')
        body = child.with_name('body.bin')
        if body.exists() and kind in ('PE32 image', 'TE image', 'DXE dependency', 'Raw'):
            if kind in sections:
                raise ValueError(f'Duplicate section {kind} in {ui}')
            sections[kind] = body.read_bytes()
    code_type = next((kind for kind in ('PE32 image', 'TE image') if kind in sections), None)
    if code_type:
        if entry.get('Subtype') not in types:
            continue
        code = sections[code_type]
        if code_type == 'PE32 image':
            pe_offset = struct.unpack_from('<I', code, 0x3c)[0]
            if code[pe_offset:pe_offset + 4] != b'PE\0\0':
                raise ValueError(f'Invalid PE image: {ui}')
            machine = struct.unpack_from('<H', code, pe_offset + 4)[0]
        else:
            machine = struct.unpack_from('<H', code, 2)[0]
        if machine != 0xaa64:
            raise ValueError(f'Non-AArch64 module: {ui}')
        paths = re.findall(rb'[a-zA-Z/\\0-9_\-\.]*\.dll\b', code)
        normalized = [path.decode().replace('\\', '/').split('/AARCH64/')[-1]
                      for path in paths if b'/AARCH64/' in path.replace(b'\\', b'/')]
        if normalized:
            parts = normalized[0].split('/')
            module = parts[-3]
            relative = Path(*parts[:-3])
        else:
            module = ui.replace(' ', '_')
            relative = Path(module)
        if relative.is_absolute() or '..' in relative.parts or '/' in module or '\\' in module:
            raise ValueError('Unexpected module build path')
        destination = output / relative
        put(destination / f'{module}.efi', code)
        binary_type = 'PE32' if code_type == 'PE32 image' else 'TE'
        depex = sections.get('DXE dependency')
        text = ('# Generated from the verified sheng UEFI; vendor bytes stay outside Git.\n'
                '[Defines]\n  INF_VERSION = 0x0001001B\n'
                f'  BASE_NAME = {module}\n  FILE_GUID = {guid}\n'
                f'  MODULE_TYPE = {types[entry["Subtype"]]}\n  VERSION_STRING = 1.0\n\n'
                f'[Binaries.AARCH64]\n  {binary_type}|{module}.efi|*\n')
        if depex is not None:
            put(destination / f'{module}.depex', depex)
            text += f'  DXE_DEPEX|{module}.depex|*\n'
        put(destination / f'{module}.inf', text.encode())
        modules.append({'ui': ui, 'guid': guid, 'module_type': types[entry['Subtype']],
                        'inf': str(relative / f'{module}.inf'),
                        'sha256': hashlib.sha256(code).hexdigest(),
                        'depex_sha256': hashlib.sha256(depex).hexdigest() if depex is not None else None})
    elif entry.get('Subtype') == 'Freeform' and 'Raw' in sections:
        relative = Path(ui.replace('\\', '/').replace(' ', '_'))
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Unexpected raw filename')
        put(output / 'RawFiles' / relative, sections['Raw'])
        raw_files.append({'file': str(relative), 'guid': guid,
                          'sha256': hashlib.sha256(sections['Raw']).hexdigest()})
manifest = {'source_sha256': record['sha256'], 'source_version': 'OS3.0.305.0.WNXMIXM',
            'modules': modules, 'raw_files': raw_files, 'hardware_boot_demonstrated': False}
(output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
stock_config = output / 'RawFiles/uefiplat.cfg'
if stock_config.exists():
    shutil.copyfile(stock_config, firmware / 'uefiplat.cfg')
link = ROOT / 'uefi-sheng/Binaries/sheng'
if not link.exists() and not link.is_symlink():
    link.symlink_to(os.path.relpath(output, link.parent), target_is_directory=True)
elif not link.is_symlink() or link.resolve() != output:
    raise SystemExit('Preserve existing Binaries/sheng; generated inputs are in the cache')
print(f'{len(modules)} AArch64 modules; {len(raw_files)} raw files; original DEPEX preserved')
