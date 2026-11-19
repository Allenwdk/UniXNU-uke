#!/usr/bin/env python3
"""Qualify the stock iPadOS 27.0.1 display identity and HPD ABI from its fileset."""

import argparse
from bisect import bisect_right
import hashlib
import json
from pathlib import Path
import struct
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    parser.add_argument('--core', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.core / 'kits/XnuPrelink'))
    from kc_image import Image, scan

    data = args.kernelcache.read_bytes()
    legacy = json.loads(Path(__file__).with_name('legacy-display-24A446.json').read_text())
    digest = hashlib.sha256(data).hexdigest()
    if digest != legacy['kernelcache_sha256']:
        parser.error('kernelcache does not match the 24A446 Legacy ABI fixture')
    root = Image(data)
    graphics = Image(data, root.entries['com.apple.iokit.IOMobileGraphicsFamily'][1])
    ap = Image(data, root.entries['com.apple.iokit.IOMobileGraphicsFamily-DCP'][1])
    manager = Image(data, root.entries['com.apple.driver.AppleDisplayManager'][1])
    fixups = root.fixups()

    def string_callers(image, needle):
        text = next(s for s in image.sections if s['name'] == '__text')
        _, refs, _ = scan(image.read(text['va'], text['size']), text['va'])
        starts = image.function_starts()
        return sorted({
            starts[bisect_right(starts, pc) - 1] for pc, address, kind in refs
            if kind != 'ldst' and needle in (image.cstring(address) or '')
        })

    identity_slots = (186, 187, 265, 266, 267)
    legacy_base = int(legacy['vtable'], 16)
    discriminators = {slot: int(legacy['slots'][slot][1], 16) for slot in identity_slots}

    def targets(base):
        values = {}
        for slot in identity_slots:
            target, auth, key, disc, addrdiv = fixups.get(base + slot * 8, (0, 0, 0, 0, 0))
            if (auth, key, disc, addrdiv) != (1, 0, discriminators[slot], 1):
                return {}
            values[slot] = target
        return values

    # The stock AP class' framebuffer ID slot reads its instance ID. Locate
    # its table using the same PAC signatures as the existing Legacy provider.
    ap_tables = {}
    for address, (_, auth, key, disc, addrdiv) in fixups.items():
        if (auth, key, disc, addrdiv) != (1, 0, discriminators[265], 1):
            continue
        start = address - 8 * 265
        if not ap.section(start):
            continue
        slots = targets(start)
        if slots and ap.read(slots[265], 12) == bytes.fromhex('5f2403d5009840b9c0035fd6'):
            ap_tables[start] = slots
    assert len(ap_tables) == 1
    ap_base, ap_targets = next(iter(ap_tables.items()))
    legacy_targets = targets(legacy_base)
    assert legacy_targets == {slot: int(legacy['slots'][slot][0], 16) for slot in identity_slots}
    assert graphics.read(legacy_targets[265], 12) == bytes.fromhex('5f2403d520028052c0035fd6')
    assert ap.read(ap_targets[266], 16) == bytes.fromhex('5f2403d508284e3900010012c0035fd6')

    hotplug, = string_callers(ap, 'virtual void IOMobileFramebufferAP::hotPlug_notify_gated(')
    power, = string_callers(graphics, 'fId %u second_external %d powerstate %d fdisplaytype %d')
    enumeration, = string_callers(manager, 'No external display connection found for fbId=')
    # These authenticated callsites use the same slots found in both tables.
    # The log anchors qualify the callers without fabricating C++ names for
    # unnamed vtable entries.
    def function(image, address):
        starts = image.function_starts()
        return image.read(address, starts[bisect_right(starts, address)] - address)

    power_code = function(graphics, power)
    enumeration_code = function(manager, enumeration)
    for slot in (265, 266, 267):
        # movk x16/x17, #<PAC discriminator>, lsl #48 precedes blraa.
        op = 0xf2e00010 | discriminators[slot] << 5
        assert struct.pack('<I', op) in power_code or struct.pack('<I', op + 1) in power_code
    op = 0xf2e00011 | discriminators[265] << 5
    assert struct.pack('<I', op) in enumeration_code

    # Selector 23 forwards the color ID followed by the timing ID. The
    # element ordering is confirmed by the two same-boot mode requests.
    # The Legacy selector-25 gated body returns the native digital-out state.
    # Type 0 is the first list of the Legacy notification registration body.
    mode_call = function(graphics, 0xfffffe000a280ad8)
    assert bytes.fromhex('010140b9020940b9') in mode_call
    assert struct.pack('<I', 0xf2e00011 | discriminators[187] << 5) in mode_call
    # Selector 79 passes a buffer and size to the original get-block slot.
    mode_query_slot = 229
    mode_query_target, mode_query_diversity = (int(x, 16) for x in legacy['slots'][mode_query_slot])
    assert fixups[legacy_base + mode_query_slot * 8] == (mode_query_target, 1, 0, mode_query_diversity, 1)
    assert graphics.read(mode_query_target, 16) == bytes.fromhex('5f2403d5e05880520000bc72c0035fd6')
    assert graphics.read(0xfffffe000a2800f4, 40) == bytes.fromhex(
        '11e580d21002118b0a0240f9eb0310aaf0030aaae00309aae20308aaf1030baa113ee1f2110a1fd7')
    assert graphics.read(0xfffffe000a28d99c, 20) == bytes.fromhex('5f2403d508b85db9280000b900008052c0035fd6')
    assert graphics.read(0xfffffe000a292620, 12) == bytes.fromhex('084d83526802088b09018052')
    assert ap.read(0xfffffe000a255fd8, 20) == bytes.fromhex('20630091e2830091010080524300805224008052')
    assert legacy['display']['digital_state'] == 0x1db8
    assert legacy['display']['hpd_clients'] == 0x1a68

    ioav = Image(data, root.entries['com.apple.iokit.IOAVFamily'][1])
    binding_prefixes = {
        '_native_legacy_send_notification': (graphics, '5f2403d5856c0014'),
        '_native_legacy_ioav_timing': (ioav, '5f2403d57f2303d5f657bda9f44f01a9fd7b02a9fd830091f40302aaf50301aaf60300aa'),
        '_native_legacy_ioav_timing_colors': (ioav, '5f2403d57f2303d5f657bda9f44f01a9fd7b02a9fd830091f30301aaf40300aa210100b4'),
        '_native_legacy_ioav_color': (ioav, '5f2403d57f2303d5ffc300d1fd7b02a9fd830091e80300aaa92b41a9ab1340f9'),
    }
    for symbol, (image, expected) in binding_prefixes.items():
        address = int(legacy['bindings'][symbol], 16)
        code = bytes.fromhex(expected)
        assert image.read(address, len(code)) == code, symbol

    surface = Image(data, root.entries['com.apple.iokit.IOSurface'][1])
    # Both accessors bound the unsigned plane index before loading an
    # entry. The IOSurface parser writes PlaneBase at +0x10 and
    # PlaneBytesPerRow at +0x20 in each 56-byte entry.
    plane_bindings = {
        '_native_legacy_surface_plane_base': (0xfffffe000a43325c, 0xfffffe000a4332f8, '000940f9'),
        '_native_legacy_surface_plane_stride': (0xfffffe000a4337f0, 0xfffffe000a43388c, '001140f9'),
    }
    for symbol, (address, field_load, code) in plane_bindings.items():
        assert int(legacy['bindings'][symbol], 16) == address, symbol
        assert surface.read(address, 4) == bytes.fromhex('5f2403d5'), symbol
        assert surface.read(field_load, 4) == bytes.fromhex(code), symbol
    assert surface.read(0xfffffe000a42c880, 4) == bytes.fromhex('48831ff8')

    print(json.dumps({
        'build': '24A446',
        'kernelcache_sha256': digest,
        'stock_legacy_vtable_address_point': hex(legacy_base),
        'stock_ap_vtable_address_point': hex(ap_base),
        'identity_slots': {
            str(slot): {'diversity': hex(discriminators[slot]), 'legacy': hex(legacy_targets[slot]),
                        'ap': hex(ap_targets[slot])}
            for slot in identity_slots
        },
        'ap_hotplug_gated': hex(hotplug),
        'graphics_power_request': hex(power),
        'display_manager_external_enumeration': hex(enumeration),
        'legacy_mode_and_hpd_verified': True,
        'legacy_get_block_slot': {'slot': mode_query_slot, 'diversity': hex(mode_query_diversity)},
        'ioav_bindings_verified': sorted(binding_prefixes),
        'iosurface_plane_bindings_verified': sorted(plane_bindings),
    }, indent=2))


if __name__ == '__main__':
    main()
