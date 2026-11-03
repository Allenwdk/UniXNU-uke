#!/usr/bin/env python3
"""Pack uke's compressed payload with the upstream Android vendor_boot v4 packer."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', required=True, type=Path)
p.add_argument('--stock', required=True, type=Path)
p.add_argument('--payload', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
sys.path.insert(0, str(a.core.resolve() / 'scripts'))
from unixnu import vendor_boot
stock = a.stock.read_bytes()
parts = vendor_boot.parse(stock)
image, report = vendor_boot.pack(parts, a.payload.read_bytes())
# Keep the original vbmeta and footer while omitting unused partition padding.
if parts['vbmeta']:
    end = vendor_boot.align(report['vbmeta_offset'] + len(parts['vbmeta']) + 64)
    compact = bytearray(image[:end])
    compact[-64:] = image[-64:]
    image = bytes(compact)
else:
    end = vendor_boot.align(report['bootconfig_offset'] + len(parts['bootconfig']))
    image = image[:end]
if len(image) >= 96 * 1024 * 1024:
    raise SystemExit('uke vendor_boot must fit below 96 MiB')
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_bytes(image)
report.update(image_bytes=len(image), image_sha256=hashlib.sha256(image).hexdigest(),
              stock_sha256=hashlib.sha256(stock).hexdigest(), hardware_tested=False)
a.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'{a.output}: {len(image)} bytes; original DTB, table fields, bootconfig and vbmeta retained')
