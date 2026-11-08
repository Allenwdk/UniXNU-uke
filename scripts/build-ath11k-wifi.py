#!/usr/bin/env python3
"""Compile the ported ath11k AHB stack with the device toolchain.
Compiles the shared Linux shim/mac80211/cfg80211 objects, the ath11k
core/AHB objects and src/nw_ahb.c; reports a link audit. The full link into
the kernel is done by the board build script."""
import argparse, hashlib, json, re, subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

def stage_rx_alignment(source: Path, out: Path) -> Path:
    # Backport Mark Ruvald Pedersen's ath11k RFC PATCH 1/3, July 19, 2026:
    # https://lore.kernel.org/ath11k/20260719215817.2468580-2-wabsie@gmail.com/
    # The refill and monitor-status skbs are empty. Reserve moves both data
    # and tail; pull cannot consume a positive length from an empty skb.
    aligned, count = re.subn(
        r"skb_pull(?=\(skb,\s+PTR_ALIGN\(skb->data, DP_RX_BUFFER_ALIGN_SIZE\))",
        "skb_reserve", source.read_text(),
    )
    if count != 2:
        raise SystemExit(f"expected two ath11k RX alignment sites, found {count}")
    staged = out / "ath11k-src" / source.name
    staged.parent.mkdir(exist_ok=True)
    staged.write_text(aligned)
    return staged


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
    skip = {'src/nw_mhi.c', 'src/nw_pci.c', 'src/nw_qrtr_mhi.c', 'src/nw_debug.c'}
    sources = []
    for line in (module / 'tools/sources.txt').read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#') or line in skip: continue
        if line.startswith('ath12k:') or line.startswith('mhi:'): continue
        tree, _, name = line.partition(':') if ':' in line else ('', '', line)
        if tree == 'qmi':
            base = module / 'deps/linux/drivers/soc/qcom'
        elif tree == 'generated':
            base = module / 'out'
        elif tree in ('mac80211', 'cfg80211', 'crypto'):
            base = module / 'out' / tree
        else:
            base = module
        sources.append(base / name)
    ath = json.loads((module / 'out/ath11k-identity.json').read_text())
    ath_src = module / 'out/ath11k-src'
    aligned_rx = stage_rx_alignment(ath_src / 'dp_rx.c', out)
    sources += [aligned_rx if n == 'dp_rx.c' else ath_src / n for n in ath['sources']]
    sources.append(module / 'out/ath11k-src/reg.c')
    # The board's ring inventory now lives in the module (src/nw_diag_ahb.c), so
    # the stack owns the diagnostics the ABI publishes and the board only calls
    # nw_wifi_ring_state().
    missing = [str(source) for source in sources if not source.is_file()]
    if missing:
        raise SystemExit("missing ath11k build sources: " + ", ".join(missing))
    def object_name(src):
        parts = src.parts
        for tag, prefix in (('ath11k-src', 'ath11k_'), ('mac80211', 'mac80211_'), ('cfg80211', 'cfg80211_'), ('crypto', 'crypto_')):
            if tag in parts: return prefix + src.stem + '.o'
        return src.stem + '.o'
    for stale in out.glob('*.o'):
        stale.unlink()
    def build(src):
        obj = out / object_name(src)
        extra = ['-I', str(module / 'out/cfg80211')] if src.name == 'nw_cfg80211_native.c' else []
        r = subprocess.run([clang, *extra, *flags, '-c', str(src), '-o', str(obj)], capture_output=True, text=True)
        return src, obj, r
    with ThreadPoolExecutor(8) as pool:
        results = list(pool.map(build, sources))
    failed = [(s, r) for s, _, r in results if r.returncode]
    for s, r in failed[:12]:
        print(s.name, 'FAILED:', next((l for l in r.stderr.splitlines() if 'error:' in l), r.stderr[:160]))
    if failed:
        raise SystemExit(f"ath11k compilation failed for {len(failed)} source files")
    objs = [o for _, o, r in results if not r.returncode]
    keep = {'_nw_ahb_start', '_nw_ahb_stop', '_nw_ahb_qrtr_input', '_nw_ahb_qrtr_down', '_nw_ahb_qrtr_up',
            '_lx_plat', '_lx_pump', '_lx_irq_raise', '_lx_irq_polling', '_ath11k_debug_mask',
            '_nw_wifi_start', '_nw_wifi_step', '_nw_wifi_stop', '_nw_wifi_stats', '_nw_wifi_set_events',
            '_nw_wifi_scan', '_nw_wifi_bss_count', '_nw_wifi_bss_get', '_nw_wifi_connect', '_nw_wifi_disconnect',
            '_nw_wifi_tx', '_nw_wifi_status_get', '_nw_wifi_where', '_nw_wifi_tx_reasons',
            # Station boundary the board drives from its worker (nw_mainline.c is
            # now part of the stack): init/run/poll/pending plus the station calls.
            '_nw_mainline_init', '_nw_mainline_run', '_nw_mainline_poll', '_nw_mainline_pending',
            '_nw_mainline_shutdown', '_nw_mainline_rate_update', '_nw_mainline_signal_update',
            '_nw_wifi_tx_packet', '_nw_wifi_stats', '_nw_wifi_hw_ready', '_nw_cfg80211_primary',
            '_nw_cfg80211_selftest_scan',
            # The board starts the stack itself: cfg80211/mac80211 initcalls and the
            # root device they publish into (see WifiAth11k.cpp).
            '_lx_root_init', '_lx_root_device', '_lx_init_cfg80211_init', '_lx_init_ieee80211_init', '_lx_init_regulatory_init_db',
            # Control endpoint: the board's native socket calls these and the
            # station layer polls the queue. nw_control.c holds them behind the
            # transport installed before cfg80211's nl80211_init binds dispatch.
            '_nw_wifi_control_transport_set', '_nw_wifi_control_bind', '_nw_wifi_control_attach',
            '_nw_wifi_control_detach', '_nw_wifi_control_receive', '_nw_wifi_control_writable',
            '_nw_wifi_control_publish', '_nw_wifi_control_poll', '_nw_wifi_control_pending',
            '_nw_wifi_control_stats_get', '_nw_wifi_control_shutdown',
            # Worker boundary for the module's own lifecycle (nw_wifi.c built for
            # NW_PLATFORM_AHB) and the receiver/transmit diagnostics WifiNet reads.
            '_nw_wifi_poll', '_nw_wifi_wait_ms', '_nw_wifi_irq_mode', '_nw_wifi_irq_pending',
            '_nw_wifi_ring_state', '_nw_wifi_rx_state', '_nw_wifi_link_state',
            # Board-visible lifecycle state and the wire codec the board's
            # control self-test builds its INFO request with. Every symbol the
            # board references must keep its name: objcopy renames the rest to
            # _nwk* and only module-internal references follow that rename.
            '_nw_wifi_set_platform', '_nw_board', '_nw_control_begin', '_nw_control_decode',
            # The network side (WifiNet.cpp) is part of the same archive and calls
            # these across objects; they must keep their names too.
            '_nw_wifi_buffer_stats_get', '_nw_wifi_power',
            # Board thread housekeeping: the receive rings must be replenished while
            # the port has no data path interrupt (see dp_rx.c).
            '_ath11k_dp_rx_poll_replenish'}
    names = set()
    for o in objs:
        for line in subprocess.check_output(['llvm-nm', '-g', '--defined-only', str(o)], text=True).splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[2] not in keep: names.add(fields[2])
    mapping = out / 'rename.txt'
    mapping.write_text(''.join(f'{n} _nwk{n}\n' for n in sorted(names)))
    for o in objs:
        subprocess.run(['llvm-objcopy', '--redefine-syms=' + str(mapping), str(o)], check=True)
    report = {'built': len(objs), 'total': len(sources), 'failed': [str(s) for s, _ in failed],
              'objects': [str(o) for o in objs], 'renamed_globals': len(names), 'linked_backend': False, 'hardware_verified': False}
    report['diag_source_sha256'] = hashlib.sha256((module / 'src/nw_diag_ahb.c').read_bytes()).hexdigest()
    report['lifecycle_source_sha256'] = hashlib.sha256((module / 'src/nw_wifi.c').read_bytes()).hexdigest()
    report['rx_alignment'] = {
        'base_sha256': hashlib.sha256((ath_src / 'dp_rx.c').read_bytes()).hexdigest(),
        'staged_sha256': hashlib.sha256(aligned_rx.read_bytes()).hexdigest(),
        'object_sha256': hashlib.sha256((out / 'ath11k_dp_rx.o').read_bytes()).hexdigest(),
    }
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'ath11k stack: {len(objs)}/{len(sources)} objects')
if __name__ == '__main__':
    main()
