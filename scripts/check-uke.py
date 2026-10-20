#!/usr/bin/env python3
"""Check candidate platform intervals against the compiled uke device tree."""
import json
import csv
import os
from pathlib import Path
import re
import subprocess
import tomllib

UE = Path(os.environ.get('UNIXNU_UEFI', 'uefi-uke'))
PLATFORM = UE / 'Platforms/Xiaomi/ukePkg'
DTB = Path(os.environ.get('UNIXNU_DTB', 'build/uke/uke.dtb'))


def cells(node, prop):
    result = subprocess.run(['fdtget', '-t', 'x', str(DTB), node, prop],
                            capture_output=True, text=True)
    if result.returncode:
        return []
    return [int(word, 16) for word in result.stdout.split()]


if __name__ == '__main__':
    nodes = subprocess.check_output(['fdtget', '-l', str(DTB), '/reserved-memory'], text=True).split()
    reservations = []
    for node in nodes:
        values = cells('/reserved-memory/' + node, 'reg')
        if not values or node == 'ramoops_region':
            continue
        # Static reservations are the no-map nodes. splash_region is a fixed
        # display carveout even though it is not no-map; ramoops and the CMA
        # pools are post-boot consumers of memory UEFI owns during boot.
        no_map = subprocess.run(['fdtget', '-t', 's', str(DTB),
                                 '/reserved-memory/' + node, 'no-map'],
                                capture_output=True).returncode == 0
        if not no_map and node != 'splash_region':
            continue
        for index in range(0, len(values), 4):
            base = (values[index] << 32) | values[index + 1]
            size = (values[index + 2] << 32) | values[index + 3]
            if not size:
                continue
            # The UEFI memory descriptor table is 4 KiB aligned.
            base &= ~0xFFF
            size = (size + 0xFFF) & ~0xFFF
            reservations.append((node, base, size))
    source = (PLATFORM / 'Library/MemoryMapLib/MemoryMapLib.c').read_text()
    rows = [(m[1], int(m[2], 16), int(m[3], 16), m[4]) for m in re.finditer(
        r'\{"([^"]+)"\s*,\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),([^}]+)\}', source)]
    errors = []
    if not rows or len(rows) > 128:
        errors.append('Empty map or descriptor capacity exceeded')
    for name, base, size in reservations:
        if not any(a == base and s == size and re.search(r'\bReserv\b', attrs)
                   for _, a, s, attrs in rows):
            errors.append(f'Missing reservation: {name}')
    for index, (name, base, size, _) in enumerate(rows):
        if base % 4096 or size % 4096 or not size:
            errors.append(f'Invalid UEFI page alignment: {name}')
        for other, other_base, other_size, _ in rows[index + 1:]:
            if max(base, other_base) < min(base + size, other_base + other_size):
                errors.append(f'Overlapping intervals: {name}, {other}')
    config = tomllib.loads((UE / 'Resources/Configs/uke.toml').read_text())
    fd = config['uefi_fd']
    if not any(n == 'UEFI FD' and a == fd['base'] and s == fd['size'] for n, a, s, _ in rows):
        errors.append('FD configuration does not match memory descriptor')
    dsc = (PLATFORM / 'uke.dsc').read_text()
    stack = re.search(r'PcdCPUCoresStackBase\|(0x[0-9a-fA-F]+)', dsc)
    if not stack or not any(n == 'UEFI Stack' and a == int(stack[1], 16) for n, a, _, _ in rows):
        errors.append('Stack configuration does not match memory descriptor')
    dependencies = set()
    hw = re.search(r'UKE_VENDOR_HW\s*=\s*(\d+)', dsc)
    if not hw or int(hw.group(1)) != 0:
        for inc in (PLATFORM / 'Include').glob('*.inc'):
            dependencies.update(re.findall(r'Binaries/uke/[^\s}]+', inc.read_text()))
    missing = sorted(p for p in dependencies if not (UE / p).is_file())
    firmware = Path('.cache/firmware/uke/uefiplat.cfg')
    stock_count = 0
    if firmware.exists() and '[MemoryMap]' in firmware.read_text(errors='replace'):
        text = firmware.read_text(errors='replace').split('[MemoryMap]', 1)[1].split('[RegisterMap]', 1)[0]
        for line in text.splitlines():
            if not line.strip().startswith('0x'):
                continue
            record = next(csv.reader([line], skipinitialspace=True))
            base, size = int(record[0], 16), int(record[1], 16)
            remaining = [(base, base + size)]
            stock_count += 1
            for name, address, extent, attributes in rows:
                remaining = [piece for lo, hi in remaining
                             for piece in [(lo, min(hi, address)), (max(lo, address + extent), hi)]
                             if piece[0] < piece[1]]
                if max(base, address) < min(base + size, address + extent):
                    if record[3] == 'NoMap' and 'HobOnlyNoCacheSetting' not in attributes:
                        errors.append(f'Stock NoMap range is mapped: {name}')
                    if record[6] == 'Reserv' and not re.search(r'\bReserv\b', attributes):
                        errors.append(f'Stock reserved range is allocatable: {name}')
            if remaining:
                errors.append(f'Uncovered stock region: {record[2]}')
    result = {'descriptors': len(rows), 'source_reservations': len(reservations),
              'stock_regions_checked': stock_count,
              'static_errors': errors, 'missing_vendor_inputs': missing,
              'runtime_ram_verified': False, 'hardware_boot_demonstrated': False}
    Path('build/uke/check.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(reservations)} DTS reservations checked against {len(rows)} descriptors')
    print(f'{len(errors)} static errors; {len(missing)} vendor inputs still missing; hardware unverified')
    for error in errors:
        print(error)
    raise SystemExit(bool(errors))
