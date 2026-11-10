#!/usr/bin/env python3
"""Check the real 24A446 imports, append-only header rewrite and owner signatures."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def commands(raw):
    at, found = 32, []
    for _ in range(struct.unpack_from('<I', raw, 16)[0]):
        command, size = struct.unpack_from('<II', raw, at)
        found.append((command, at, size, raw[at:at + size]))
        at += size
    return found


def imports(raw):
    libraries, special = [], {}
    for command, at, length, _ in commands(raw):
        if command in (0xc, 0x80000018, 0x8000001f, 0x80000023):
            offset = struct.unpack_from('<I', raw, at + 8)[0]
            libraries.append(raw[at + offset:at + length].split(b'\0', 1)[0].decode())
        if command == 2:
            table, count, strings, _ = struct.unpack_from('<4I', raw, at + 8)
    for index in range(count):
        offset, kind, section, desc, va = struct.unpack_from('<IBBHQ', raw, table + index * 16)
        if not offset:
            continue
        name = raw[strings + offset:raw.index(0, strings + offset)].decode()
        if name == '_OBJC_CLASS_$_NSConstantDoubleNumber':
            ordinal = desc >> 8
            special[name] = dict(ordinal=ordinal, library=libraries[ordinal - 1])
    return libraries, special


def sections(raw):
    found = {}
    for command, at, length, _ in commands(raw):
        if command != 0x19:
            continue
        for index in range(struct.unpack_from('<I', raw, at + 64)[0]):
            start = at + 72 + index * 80
            va, size, fileoff = struct.unpack_from('<QQI', raw, start + 32)
            if fileoff:
                found[raw[start:start + 32].hex()] = (va, size, fileoff,
                    hashlib.sha256(raw[fileoff:fileoff + size]).hexdigest())
    return found


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--original', type=Path, required=True)
    p.add_argument('--runtime', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    helper = load('backlight_macho', ROOT.parent / 'UniXNU/scripts/appsync/macho.py')
    runtime = load('backlight_runtime', ROOT / 'scripts/build-backlight-runtime.py')
    original = args.original.read_bytes()
    pin = hashlib.sha256(original).hexdigest()
    rewritten = helper.append_dylib(original, runtime.DYLIB, pin)
    oldcommands, newcommands = commands(original), commands(rewritten)
    assert [item[3] for item in oldcommands] == [item[3] for item in newcommands[:-1]]
    end = 32 + struct.unpack_from('<I', original, 20)[0]
    newend = 32 + struct.unpack_from('<I', rewritten, 20)[0]
    assert original[newend:] == rewritten[newend:]
    assert original[:16] == rewritten[:16] and original[24:end] == rewritten[24:end]
    original_libraries, symbols = imports(original)
    libraries, newsymbols = imports(rewritten)
    assert libraries[:-1] == original_libraries and newsymbols == symbols
    assert symbols['_OBJC_CLASS_$_NSConstantDoubleNumber']['library'] == '/System/Library/Frameworks/Foundation.framework/Foundation'
    signed = (args.runtime / 'libexec/backboardd').read_bytes()
    assert imports(signed) == (libraries, symbols)
    assert sections(original) == sections(signed)
    sig = runtime.signature(signed)
    before = runtime.signature(original)
    assert sig['identifier'] == before['identifier'] and sig['entitlements'] == before['entitlements']
    runtime.require_adhoc({'backboardd': sig, 'library': runtime.signature((args.runtime / 'lib/UniXNUBacklight.dylib').read_bytes())})
    negatives = []
    def reject(data, name, expected, label):
        try:
            helper.append_dylib(data, name, expected)
        except ValueError:
            negatives.append(label)
        else:
            raise AssertionError(label)
    reject(original, runtime.DYLIB, '0' * 64, 'wrong-pin')
    reject(original[:20], runtime.DYLIB, hashlib.sha256(original[:20]).hexdigest(), 'short-header')
    reject(rewritten, runtime.DYLIB, hashlib.sha256(rewritten).hexdigest(), 'duplicate-library')
    bad = bytearray(original)
    bad[end] = 0x55
    reject(bad, runtime.DYLIB, hashlib.sha256(bad).hexdigest(), 'nonzero-padding')
    reject(original, '/' + 'x' * 256, pin, 'insufficient-padding')
    reject(original, 'relative.dylib', pin, 'relative-library')
    for offset in (16, 20):
        bad = bytearray(original)
        struct.pack_into('<I', bad, offset, 0xfffffff0)
        reject(bad, runtime.DYLIB, hashlib.sha256(bad).hexdigest(), f'bad-header-field-{offset}')
    bad = bytearray(signed)
    bad[0x1000] ^= 1
    try:
        runtime.signature(bad)
    except ValueError:
        negatives.append('corrupted-signed-page')
    else:
        raise AssertionError('corrupted signed page accepted')
    # The former dependency replacement must fail the import-preservation rule.
    prior = helper.add_dylib(original, runtime.DYLIB, pin)
    assert imports(prior)[1] != symbols
    negatives.append('former-foundation-replacement-detected')
    report = dict(original_dependencies_preserved=True, original_sections_preserved=True,
                  original_commands_preserved_before_signing=True, added_command_bytes=newend - end,
                  constant_double_import=symbols, negatives=negatives, signature=sig, hardware_verified=False)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS runtime dependency/signature regression, {len(negatives)} negative cases; Foundation ordinal preserved')


if __name__ == '__main__':
    main()
