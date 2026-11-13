#!/usr/bin/env python3
"""Build upstream 24A446 AppSync on Linux, preserving Uke's addon and trust."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--addon', type=Path, required=True)
    parser.add_argument('--trust', type=Path, required=True)
    parser.add_argument('--core', type=Path, default=ROOT.parent / 'UniXNU')
    parser.add_argument('--sdk', type=Path, default=Path.home() / '.cache/ios-sdks/iPhoneOS17.0.sdk')
    parser.add_argument('--compiler', default='/usr/bin/clang')
    parser.add_argument('--linker', type=Path, default=Path.home() / '.local/share/unixnu-startup-lto/toolchain-gpu/bin/ld')
    parser.add_argument('--ldid', default=str(Path.home() / '.cache/ldid/ldid'))
    args = parser.parse_args()
    core, out = args.core.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(core / 'scripts/appsync'))
    upstream = load('uke_upstream_appsync', core / 'scripts/appsync/build.py')
    signing = load('uke_runtime_signatures', ROOT / 'scripts/build-backlight-runtime.py')
    trust_helper = load('uke_trust_modules', ROOT / 'scripts/build-wifi-trust.py')

    def metadata(path):
        return signing.signature(Path(path).read_bytes())

    def sign(path, identifier=None, entitlement_file=None):
        # Preserve the original private service's identifier and entitlements.
        source = next((source for source, (dest, _) in upstream.PINS.items()
                       if str(path).endswith('/' + dest)), None)
        before = metadata(args.os_root / source) if source and identifier is None else None
        identity = identifier or before['identifier']
        temporary = None
        if before and entitlement_file is None:
            temporary = tempfile.NamedTemporaryFile(suffix='.plist', dir=out)
            temporary.write(plistlib.dumps(before['entitlements']))
            temporary.flush()
            entitlement_file = temporary.name
        try:
            subprocess.run([args.ldid, '-Hsha256', '-P12', '-Cadhoc',
                            '-S' + str(entitlement_file) if entitlement_file else '-S',
                            '-I' + identity, str(path)], check=True)
            after = metadata(path)
            signing.require_adhoc({str(path): after})
            if after['identifier'] != identity or (before and after['entitlements'] != before['entitlements']):
                raise ValueError('AppSync service identity or entitlements changed')
        finally:
            if temporary:
                temporary.close()

    upstream.sign = sign
    upstream.details = metadata
    upstream.entitlements = lambda path: metadata(path)['entitlements']
    # Use the production upstream builder and signer without codesign/xcrun shims
    # that silently accept signatures. Only compiler/tool discovery is adapted.
    real_run = upstream.run

    def run(*values, **kwargs):
        if values[:2] == ('xcrun', 'clang') and '-dynamiclib' in values:
            values = (*values, ROOT / 'package/UkeBrightnessTransactions.m')
        return real_run(*values, **kwargs)

    upstream.run = run
    with tempfile.TemporaryDirectory(prefix='appsync-tools-', dir=out) as temporary:
        tool = Path(temporary) / 'xcrun'
        # OpenSSL invokes CC='xcrun clang' internally. Keep this bridge local to
        # this build, including the cache key's actual compiler/linker identity.
        tool.write_text('''#!/usr/bin/env python3
import os, sys
args = sys.argv[1:]
if args[:3] == ['--sdk', 'iphoneos', '--show-sdk-path']:
    print(os.environ['UKE_APPSYNC_SDK']); sys.exit(0)
name, *args = args
if name in ('clang', 'clang++'):
    compiler = os.environ['UKE_APPSYNC_CC'] + ('++' if name == 'clang++' else '')
    args = ['--ld-path=' + os.environ['UKE_APPSYNC_LD'], *args]
    if '-dynamiclib' in args or ('-c' not in args and '--version' not in args and '-E' not in args):
        args += ['-Wl,-fixup_chains']
    if any(str(arg).endswith('.m') for arg in args):
        args += ['-fblocks', '-Xclang', '-fptrauth-block-descriptor-pointers']
    os.execv(compiler, [compiler, *args])
if name not in ('ar', 'ranlib', 'nm'):
    raise SystemExit('unsupported AppSync build tool: ' + name)
os.execv('/usr/bin/llvm-' + name, ['llvm-' + name, *args])
''')
        tool.chmod(0o755)
        old = dict(os.environ)
        try:
            os.environ.update(PATH=str(tool.parent) + os.pathsep + old['PATH'],
                              UKE_APPSYNC_SDK=str(args.sdk.resolve()), UKE_APPSYNC_CC=args.compiler,
                              UKE_APPSYNC_LD=str(args.linker.resolve()))
            upstream.build(args.os_root.resolve(), out)
        finally:
            os.environ.clear()
            os.environ.update(old)

    report = json.loads((out / 'report.json').read_text())
    # Include the adaptation license alongside the signer's dependency licenses.
    shutil.copyfile(core / 'scripts/appsync/LICENSE', out / 'libexec/appsync-licenses/AppSync-COPYING')
    directories = [str(value) for prefix, directory in report['directories'].items()
                   for value in (prefix, out / directory)]
    subprocess.run([sys.executable, str(core / 'scripts/addon-add-dir.py'), str(args.addon),
                    str(out / 'gpuaddon.bin'), *directories], check=True)
    signatures = {name: metadata(out / name) for name in report['files']}
    services = {dest for source, (dest, _) in upstream.PINS.items()}
    entries = {}
    for name, info in signatures.items():
        entries[bytes.fromhex(info['cdhash'])] = bytes([2, 0, 5 if name in services else 0, 0])
    # Nested Apple system executables are copied byte-for-byte and already in
    # the preserved OS trust modules. They must not be owner re-signed.
    original = trust_helper.modules(args.trust.read_bytes())
    for module in original:
        if any(module[at:at + 20] in entries for at in range(24, len(module), 24)):
            raise ValueError('AppSync CDHash already has a competing trust role')
    identity = uuid.uuid5(uuid.NAMESPACE_URL, 'org.unixnu.uke.appsync:' + ''.join(key.hex() for key in sorted(entries))).bytes
    owner = struct.pack('<I16sI', 2, identity, len(entries))
    owner += b''.join(key + entries[key] for key in sorted(entries))
    modules = [*original, owner]
    cursor, offsets = 4 + 4 * len(modules), []
    for module in modules:
        offsets.append(cursor)
        cursor += len(module)
    bundle = struct.pack(f'<{len(modules) + 1}I', len(modules), *offsets) + b''.join(modules)
    if trust_helper.modules(bundle)[:-1] != original:
        raise ValueError('original trust modules changed')
    (out / 'owner-trust.bundle').write_bytes(bundle)
    report.update(signatures=signatures, original_trust_modules_preserved=True,
                  service_constraint_category=5, library_constraint_category=0,
                  uke_direct_brightness_transactions=True, hardware_verified=False,
                  upstream_commits=['c3c7468ad9f719b54f1011bd3bb01eea25a5ed5a',
                                    '20cf25def67cc12ef9339553294e1617b7d9e94e'])
    report['runtime_sha256'] = {name: hashlib.sha256((out / name).read_bytes()).hexdigest()
                                for name in ('gpuaddon.bin', 'owner-trust.bundle')}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['runtime_sha256'], indent=2))


if __name__ == '__main__':
    main()
