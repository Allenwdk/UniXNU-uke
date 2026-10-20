#!/usr/bin/env python3
"""Reconcile the pinned board candidate with verified stock UEFI reservations."""
import configparser
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
platform = ROOT / 'uefi-sheng/Platforms/Xiaomi/shengPkg'
config_file = ROOT / '.cache/firmware/sheng/uefiplat.cfg'
config = configparser.ConfigParser(interpolation=None)
config.optionxform = str
config.read_string('[ConfigParameters]\n' + config_file.read_text().split('[ConfigParameters]', 1)[1])
stock = []
for line in config_file.read_text().split('[MemoryMap]', 1)[1].split('[RegisterMap]', 1)[0].splitlines():
    if line.strip().startswith('0x'):
        row = next(csv.reader([line], skipinitialspace=True))
        stock.append((row[2], int(row[0], 16), int(row[1], 16), row[3:]))
relative = 'Platforms/Xiaomi/shengPkg/Library/MemoryMapLib/MemoryMapLib.c'
baseline = subprocess.check_output(['git', '-C', str(ROOT / 'uefi-sheng'), 'show',
                                    f'289c26de643585c502fe7e97bb251d62cec7a9ce:{relative}'], text=True)
pattern = re.compile(r'\{"([^\"]+)"\s*,\s*(0x[\da-fA-F]+),\s*(0x[\da-fA-F]+),([^}]+)\}')
rows = [(m[1], int(m[2], 16), int(m[3], 16), [s.strip() for s in m[4].split(',')])
        for m in pattern.finditer(baseline)]
# The stock register map has no PCIe windows; no PCIe driver is selected.
rows = [row for row in rows if not row[0].startswith('PCIE_')]
added = []
no_map = []
for name, address, size, attributes in stock:
    remaining = [(address, address + size)]
    for old_name, old_address, old_size, old_attributes in rows:
        remaining = [part for lo, hi in remaining
                     for part in [(lo, min(hi, old_address)), (max(lo, old_address + old_size), hi)]
                     if part[0] < part[1]]
        if attributes[0] == 'NoMap' and max(address, old_address) < min(address + size, old_address + old_size):
            old_attributes[0] = 'HobOnlyNoCacheSetting'
            no_map.append(old_name)
    if attributes[0] == 'AddDynamicMem' and remaining:
        raise SystemExit('Dynamic reservation needs runtime reconciliation; do not hard-code it')
    for index, (lo, hi) in enumerate(remaining):
        current = attributes.copy()
        if current[0] == 'NoMap':
            current[0] = 'HobOnlyNoCacheSetting'
        label = name if len(remaining) == 1 and (lo, hi) == (address, address + size) else f'{name} gap{index}'
        if name == 'NOMAP':
            label = f'NOMAP {lo:08X}'
        rows.append((label, lo, hi - lo, current))
        added.append({'name': label, 'base': hex(lo), 'size': hex(hi - lo)})
for index, (name, address, size, _) in enumerate(rows):
    if len(name) >= 32 or address % 4096 or size % 4096:
        raise SystemExit(f'Invalid region name/alignment: {name}')
    for other, second, extent, _ in rows[index + 1:]:
        if max(address, second) < min(address + size, second + extent):
            raise SystemExit(f'Overlap: {name}, {other}')
if len(rows) >= 128:
    raise SystemExit('Memory descriptor capacity exceeded')
body = '\n'.join(f'  {{"{name}", 0x{address:08X}, 0x{size:08X}, {", ".join(attributes)}}},'
                 for name, address, size, attributes in rows)
start = baseline.index('gMemoryDescriptor[] = {') + len('gMemoryDescriptor[] = {')
end = baseline.index('\n};', start)
memory_source = baseline[:start] + '\n' + body.rstrip(',') + baseline[end:]
(ROOT / 'uefi-sheng' / relative).write_text(memory_source)

overrides = {'EarlyInitCoreCnt': 1, 'NumActiveCores': 1, 'EnableMultiThreading': 0,
             'EnableMultiCoreFvDecompression': 0, 'EnableDisplayThread': 0,
             'EnableDisplayImageFv': 0, 'EnableLogFsSyncInRetail': 0}
numeric = {}
strings = {}
for key, value in config['ConfigParameters'].items():
    try:
        numeric[key] = int(value, 0)
    except ValueError:
        strings[key] = value
numeric.pop('ConfigParameterCount', None)
numeric.update(overrides)
configuration = platform / 'Library/ConfigurationMapLib/ConfigurationMapLib.c'
source = configuration.read_text()
start = source.index('gConfigurationDescriptor[] = {') + len('gConfigurationDescriptor[] = {')
end = source.index('\n};', start)
body = ',\n'.join(f'  {{"{key}", 0x{value:X}}}' for key, value in sorted(numeric.items()))
configuration.write_text(source[:start] + '\n' + body + source[end:])
report = {'stock_config_sha256': hashlib.sha256(config_file.read_bytes()).hexdigest(),
          'stock_memory_regions': len(stock), 'result_memory_regions': len(rows),
          'added_regions': added, 'unmapped_source_reservations': no_map,
          'numeric_parameters': len(numeric), 'headless_single_cpu_overrides': overrides,
          'string_parameters_not_supported_by_numeric_library': strings,
          'runtime_memory_partitions_verified': False, 'hardware_boot_demonstrated': False}
path = ROOT / 'build/sheng/firmware-reconciliation.json'
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text(json.dumps(report, indent=2) + '\n')
print(f'{len(rows)} memory descriptors; {len(added)} stock gaps protected; {len(numeric)} configuration parameters')
