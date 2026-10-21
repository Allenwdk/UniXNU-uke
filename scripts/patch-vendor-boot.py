#!/usr/bin/env python3
"""Replace the vendor ramdisk of an Android vendor_boot v4 image.

The uke ABL delivers the vendor_boot vendor ramdisk and ignores the boot image
ramdisk, so the UniXNU payload lives here. A payload size change also moves the
DTB, the vendor ramdisk table and the bootconfig; this repacks them instead of
patching bytes at a fixed offset.
"""
import argparse
import struct
from pathlib import Path


def align(value, page):
    return (value + page - 1) // page * page


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base', type=Path, required=True, help='vendor_boot v4 image to take the header/DTB/table from')
parser.add_argument('--payload', type=Path, required=True, help='vendor ramdisk (the UniXNU payload FAT image)')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()

base = args.base.read_bytes()
payload = args.payload.read_bytes()
magic, header_version, page, kernel_addr, ramdisk_addr = struct.unpack_from('<8sIIII', base, 0)
if magic != b'VNDRBOOT' or header_version != 4:
    raise SystemExit('expected an Android vendor_boot v4 image')
header_size = struct.unpack_from('<I', base, 2096)[0]
if header_size != 2128 or page < 512 or page & (page - 1):
    raise SystemExit('unexpected vendor_boot header layout')
old_ramdisk = struct.unpack_from('<I', base, 24)[0]
dtb_size = struct.unpack_from('<I', base, 2100)[0]
table_size, table_entries, table_entry_size, bootconfig_size = struct.unpack_from('<4I', base, 2112)
old_dtb = align(page + old_ramdisk, page)
old_table = align(old_dtb + dtb_size, page)
old_bootconfig = old_table + table_size
dtb = base[old_dtb:old_dtb + dtb_size]
table = bytearray(base[old_table:old_table + table_size])
bootconfig = base[old_bootconfig:old_bootconfig + bootconfig_size]
if len(dtb) != dtb_size or len(table) != table_size or len(bootconfig) != bootconfig_size:
    raise SystemExit('base image is shorter than its header describes')
if table_entries and table_entry_size >= 8:
    struct.pack_into('<II', table, 0, len(payload), 0)

new_dtb = align(page + len(payload), page)
new_table = align(new_dtb + dtb_size, page)
new_bootconfig = new_table + table_size
size = align(new_bootconfig + bootconfig_size, page)
image = bytearray(size)
image[:header_size] = base[:header_size]
struct.pack_into('<I', image, 24, len(payload))
image[page:page + len(payload)] = payload
image[new_dtb:new_dtb + dtb_size] = dtb
image[new_table:new_table + table_size] = table
image[new_bootconfig:new_bootconfig + bootconfig_size] = bootconfig
args.output.write_bytes(image)
print(f'vendor_boot {args.output}: {len(image)} bytes; ramdisk {len(payload)} at {page:#x}; '
      f'dtb {dtb_size} at {new_dtb:#x}; table at {new_table:#x}; bootconfig {bootconfig_size}')
