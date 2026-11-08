#!/usr/bin/env python3
"""Stage the native supplicant service for the board's Data addon.

The supplicant itself is built on macOS (macOS/iOS userland SDK, xcrun and
codesign; see .github/workflows/build-wifi-supplicant.yml in the device repo and
tools/build-supplicant.py in Ath12kWifi). This board script takes that signed
Mach-O and stages exactly what the addon and the XNU overlay need:

  bin/unixnu-wpa_supplicant              the signed binary (addon: wifi-bin/)
  etc/wifi/wpa_supplicant.conf           ap_scan=1; networks arrive over the control channel
  daemons/org.unixnu.wifi.supplicant.plist  the launchd job added to the shipped cache
  report.json                            CDHash, hashes and the job identity

No firmware is staged: this board's driver reads the stock WCN6750 files from the
read-only modem partition (firmware/wcn6750/manifest.json), so the addon does not
carry a WCN7850-style firmware tree.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT.parent / 'UniXNU/drivers/Ath12kWifi/out/wpa-ios/bin/wpa_supplicant'


def embedded_cdhash(path):
    """Verify every signed page and special blob, then compute the CDHash.

    The host's /usr/bin/codesign may be the XNU no-op shim. Its success is not
    evidence of signature verification, so validate the embedded data directly.
    """
    data = path.read_bytes()
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic not in (0xfeedfacf, 0xfeedface):
        raise SystemExit('not a Mach-O: ' + str(path))
    ncmds = struct.unpack_from('<I', data, 16)[0]
    offset, signature = 32, None
    for _ in range(ncmds):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x1d:  # LC_CODE_SIGNATURE
            sig_offset, sig_size = struct.unpack_from('<II', data, offset + 8)
            signature = data[sig_offset:sig_offset + sig_size]
            break
        offset += size
    if not signature:
        raise SystemExit('the supplicant carries no code signature')
    blob_magic, blob_length, count = struct.unpack_from('>III', signature, 0)
    # LC_CODE_SIGNATURE covers the padded area; the SuperBlob declares its own
    # length inside it.
    if blob_magic != 0xfade0cc0 or blob_length > len(signature) or count > 32:
        raise SystemExit('unexpected embedded signature container')
    signature = signature[:blob_length]
    for index in range(count):
        slot, slot_offset = struct.unpack_from('>II', signature, 12 + index * 8)
        if slot != 0:
            continue
        directory = signature[slot_offset:]
        length = struct.unpack_from('>I', directory, 4)[0]
        directory = directory[:length]
        limit = struct.unpack_from('>I', directory, 32)[0]
        if limit != sig_offset:
            raise SystemExit('Mach-O signature does not cover the full pre-signature image')
        spec = importlib.util.spec_from_file_location('cache_signatures', ROOT / 'scripts/build-icon-service-cache.py')
        verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(verifier)
        return verifier.verify_signature(data[:limit], signature)
    raise SystemExit('no CodeDirectory blob in the signature')


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('target', choices=('ios27',))
parser.add_argument('--binary', type=Path, default=DEFAULT_BINARY)
parser.add_argument('--expect-cdhash', help='CDHash recorded by the macOS build, checked against the file')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'report.json').unlink(missing_ok=True)

binary = args.binary.resolve()
if not binary.is_file():
    raise SystemExit(f'signed supplicant missing: {binary} (build it with the macOS workflow)')
verify = subprocess.run(['codesign', '--verify', '--strict', str(binary)], capture_output=True, text=True)
if verify.returncode:
    raise SystemExit('supplicant signature does not verify: ' + verify.stderr.strip())
cdhash = embedded_cdhash(binary)
if args.expect_cdhash and cdhash != args.expect_cdhash:
    raise SystemExit('supplicant CDHash differs from the macOS build: ' + cdhash)

for name in ('bin', 'etc/wifi', 'daemons'):
    (out / name).mkdir(parents=True, exist_ok=True)
staged = out / 'bin/unixnu-wpa_supplicant'
shutil.copy2(binary, staged)
staged.chmod(0o755)
(out / 'etc/wifi/wpa_supplicant.conf').write_text(
    '# Networks arrive from the native Wi-Fi control channel.\nap_scan=1\n')
# Use the upstream service entry point. A shell trampoline did not establish a
# working control session and hides whether launchd accepted the owner job.
job = {"Label": "org.unixnu.wifi.supplicant", "ProgramArguments": [
    "/usr/libexec/unixnu-wpa_supplicant", "-Dsheng", "-ien0",
    "-c/private/var/ngpu/etc/wifi/wpa_supplicant.conf", "-q"],
    "RunAtLoad": True, "KeepAlive": True, "ThrottleInterval": 3, "UserName": "root",
    "Umask": 0o77, "ProcessType": "Interactive",
    "StandardOutPath": "/var/log/unixnu-wifi.log", "StandardErrorPath": "/var/log/unixnu-wifi.log"}
(out / 'daemons/org.unixnu.wifi.supplicant.plist').write_bytes(plistlib.dumps(job))
files = [path for name in ('bin', 'etc/wifi', 'daemons') for path in sorted((out / name).rglob('*')) if path.is_file()]
report = {
    'built': True, 'target': args.target, 'cdhash': cdhash,
    'identifier': 'org.unixnu.wifi.supplicant',
    'binary': str(binary), 'bytes': staged.stat().st_size,
    'sha256': hashlib.sha256(staged.read_bytes()).hexdigest(),
    'firmware_staged': False,
    'launch_method': 'direct system service (upstream)',
    'embedded_signature_pages_verified': True,
    'firmware_source': 'stock modem partition (firmware/wcn6750/manifest.json)',
    'hardware_verified': False,
    'files': {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files},
}
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'staged signed native Wi-Fi service: {out}; CDHash {cdhash}')
