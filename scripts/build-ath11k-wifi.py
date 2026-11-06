#!/usr/bin/env python3
"""Compile the ported ath11k AHB stack with the device toolchain.
Compiles the shared Linux shim/mac80211/cfg80211 objects, the ath11k
core/AHB objects and src/nw_ahb.c; reports a link audit. The full link into
the kernel is done by the board build script."""
import argparse, hashlib, json, subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('module', type=Path)
    ap.add_argument('output', type=Path)
    ap.add_argument('--target', default='arm64e-apple-ios27.0.0')
    args = ap.parse_args()
    module, out = args.module.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['python3', str(module / 'tools/prepare-ath11k.py')], check=True)
    subprocess.run(['bash', str(module / 'tools/prepare.sh')], check=True)
    clang = subprocess.check_output(['xcrun', '-f', 'clang'], text=True).strip()
    res = subprocess.check_output([clang, '-print-resource-dir'], text=True).strip() + '/include'
    flags = ['-target', args.target, '-mkernel', '-O2', '-g0', '-ffreestanding', '-fno-builtin', '-fno-common',
             '-fno-strict-aliasing', '-fwrapv', '-fno-stack-protector', '-mgeneral-regs-only', '-std=gnu11',
             '-Wno-error', '-Wno-incompatible-pointer-types', '-w', '-DCONFIG_ATH11K=1', '-DCONFIG_ATH11K_AHB=1',
             '-DCONFIG_ATH11K_DEBUG=1', '-DNW_PLATFORM_AHB=1', '-DCONFIG_CFG80211=1', '-DCONFIG_MAC80211=1', '-DCONFIG_MAC80211_STA_HASH_MAX_SIZE=0', '-DCONFIG_MAC80211_RC_DEFAULT=""',
             '-nostdinc', '-isystem', str(module / 'shim/libc'), '-isystem', res, '-include', 'lx_pci.h',
             '-I', str(module / 'shim/include'), '-I', str(module / 'include'), '-I', str(module / 'out/pinned'),
             '-I', str(module / 'out/ath11k-src'), '-I', str(module / 'out'), '-I', str(module / 'out/mac80211'), '-I', str(module / 'out/cfg80211'), '-I', str(module / 'out/crypto'), '-I', str(module / 'shim/crypto'), '-I', str(module)]
    skip = {'src/nw_mainline.c', 'src/nw_mhi.c', 'src/nw_pci.c', 'src/nw_qrtr_mhi.c', 'src/nw_wifi.c', 'src/nw_debug.c', 'src/nw_cfg80211_native.c'}
    sources = []
    for line in (module / 'tools/sources.txt').read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#') or line in skip: continue
        if line.startswith('ath12k:') or line.startswith('mhi:'): continue
        tree, _, name = line.partition(':') if ':' in line else ('', '', line)
        base = module / 'out' / tree if tree in ('mac80211', 'cfg80211', 'crypto') else module
        sources.append(base / name)
    ath = json.loads((module / 'out/ath11k-identity.json').read_text())
    sources += [module / 'out/ath11k-src' / n for n in ath['sources']]
    # ath11k needs the full mac80211/cfg80211 surface, not only the ath12k subset.
    optional = {'ethtool.c', 'led.c', 'fils_aead.c', 'mesh.c', 'mesh_hwmp.c', 'mesh_pathtbl.c', 'mesh_plink.c',
                'mesh_ps.c', 'mesh_sync.c', 'mesh_pm.c', 'aead_api.c', 'aes_cmac.c'}
    for d in ('mac80211', 'cfg80211'):
        sources += [f for f in sorted((module / 'out' / d).glob('*.c')) if f.name not in optional]
    sources.append(module / 'out/ath11k-src/reg.c')
    sources = [s for s in sources if s.is_file()]
    def object_name(src):
        parts = src.parts
        for tag, prefix in (('ath11k-src', 'ath11k_'), ('mac80211', 'mac80211_'), ('cfg80211', 'cfg80211_'), ('crypto', 'crypto_')):
            if tag in parts: return prefix + src.stem + '.o'
        return src.stem + '.o'
    for stale in out.glob('*.o'):
        stale.unlink()
    def build(src):
        obj = out / object_name(src)
        r = subprocess.run([clang, *flags, '-c', str(src), '-o', str(obj)], capture_output=True, text=True)
        return src, obj, r
    with ThreadPoolExecutor(8) as pool:
        results = list(pool.map(build, sources))
    failed = [(s, r) for s, _, r in results if r.returncode]
    for s, r in failed[:12]:
        print(s.name, 'FAILED:', next((l for l in r.stderr.splitlines() if 'error:' in l), r.stderr[:160]))
    objs = [o for _, o, r in results if not r.returncode]
    report = {'built': len(objs), 'total': len(sources), 'failed': [str(s) for s, _ in failed],
              'objects': [str(o) for o in objs], 'linked_backend': False, 'hardware_verified': False}
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'ath11k stack: {len(objs)}/{len(sources)} objects')
if __name__ == '__main__':
    main()
