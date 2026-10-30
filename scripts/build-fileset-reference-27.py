#!/usr/bin/env python3
"""Build a symbol-bearing RELEASE comparison kernel for the 24A446 prelink (uke).

The 24A446 kernelcache is an MH_FILESET with stripped symbols; emit_prelink matches the source calls
against this build of the same patched source and sends unmatched ones to the reporting stub
(fileset-unmapped-call.patch). Ported from UniXNU's build-fileset-release-reference.py: the tree is the
one this script runs in, and the arch/deployment come from the build environment.
"""
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
# The reference copies .cache/xnu-work before build-xnu-4k-baseline.sh runs its Linux rewrites; apply them first
# (idempotent) so the copy gets the standard vector syntax the upstream assembler needs.
prepare = root / 'scripts/linux/prepare-xnu.py'
if prepare.is_file():
    subprocess.run(['python3', str(prepare), 'source'], cwd=root, check=True)
output = root / 'build/xnu/fileset-reference'
output.mkdir(parents=True, exist_ok=True)
source = output / 'source'
# The copy is valid only for the patch set that produced .cache/xnu-work; a re-patched tree must be copied again.
patch_list = root.parent / 'xnu-patches.list'
signature = hashlib.sha256(patch_list.read_bytes()).hexdigest() if patch_list.is_file() else ''
stamp = output / 'source-signature'
if source.exists() and (not stamp.is_file() or stamp.read_text().strip() != signature):
    shutil.rmtree(source)
if not source.exists():
    shutil.copytree(root / '.cache/xnu-work', source, symlinks=True,
                    ignore=shutil.ignore_patterns('.git'))
    # The shared tree keeps the observation hook at file scope; the reference build disables it.
    sleh = source / 'osfmk/arm64/sleh.c'
    text = sleh.read_text()
    misplaced = '\n\tboot_progress_user_breakpoint(state, esr);\n#if __has_feature(ptrauth_calls)'
    if misplaced in text:
        sleh.write_text(text.replace(misplaced, '\n#if __has_feature(ptrauth_calls)', 1))
    manifest = {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in source.rglob('*') if p.is_file()}
    (output / 'source-sha256.json').write_text(json.dumps(manifest, indent=2) + '\n')
    stamp.write_text(signature + '\n')
sdk = root / '.cache/fakeroot'
arch = os.environ.get('UNIXNU_XNU_ARCH', 'arm64e')
ios = os.environ.get('UNIXNU_IOS_VERSION', '27.0')
darwin = os.environ.get('UNIXNU_XNU_DARWIN', '25.5.0')
version = os.environ.get('UNIXNU_XNU_VERSION', '12377.121.6')
archives = [root / 'build/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a']
forced = [root / f'build/{folder}/{name}' for folder, name in (
    ('libpthread-kernel', 'libpthread_kernel.a'), ('armdt-platform', 'libArmDTPlatform.a'), ('armcpu', 'libArmCpu.a'))]
assert all(p.is_file() for p in archives + forced), 'the source archives must be built before the reference'
env = dict(os.environ, KERNEL_SDKROOT=str(sdk),
           XNU_AVAILABILITY_PL=str(sdk / 'usr/local/libexec/availability.pl'))
flags = ('-Wno-reserved-identifier -Wno-pre-c11-compat -Wno-missing-designated-field-initializers '
         '-Wno-suggest-override -Wno-unnecessary-virtual-specifier -Wno-suggest-destructor-override '
         '-Wno-ms-bitfield-padding -DXNU_NATIVE_PAN=1 -DXNU_SOURCE_CORECRYPTO14=1 -DXNU_BOOT_PROGRESS=0')
deployment = f'-miphoneos-version-min={ios}'
args = ['make', '-C', str(source), 'build', 'SDKROOT=iphoneos',
        'ARCH_CONFIGS=ARM64', 'MACHINE_CONFIGS=BCM2837', 'KERNEL_CONFIGS=DEVELOPMENT',
        'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=' + arch, 'LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY',
        'ARM64_LINK_ADDR=0xfffffff007004000', 'ARM64_PLKSEG_ADDR=0xfffffff004004000',
        f'DEPLOYMENT_TARGET_FLAGS={deployment} -DXNU_TARGET_OS_IOS',
        f'DEPLOYMENT_LINKER_FLAGS=-Wl,-ios_version_min,{ios}',
        f'RC_DARWIN_KERNEL_VERSION={darwin}', f'RC_ProjectSourceVersion={version}',
        # No LTO: the Linux ld cannot consume the bitcode objects the RELEASE LTO link expects.
        'BUILD_LTO=0', 'BOUND_CHECKS=0', 'BUILD_WERROR=0', 'MAKEJOBS=-j8',
        f'CFLAGS_EXTRA={flags}',
        'copyio.o_CFLAGS_ADD=-march=armv8.1-a', 'pcb.o_CFLAGS_ADD=-march=armv8.1-a',
        'LD_KERNEL_ARCHIVES=' + ' '.join([f'-L{sdk}/usr/local/lib/kernel', '-lfirehose_kernel',
                                          *map(str, archives), *(f'-Wl,-force_load,{p}' for p in forced)]),
        'XNU_EXTERNAL_KERNEL_ARCHIVES=' + ' '.join(map(str, archives + forced)),
        f'OBJROOT={output}', f'SYMROOT={output}/sym', f'DSTROOT={output}/dst']
if os.uname().sysname == 'Linux':
    # What xnu's makefile takes from a Mac, as build-xnu-4k-baseline.sh passes to the main build.
    clt = os.environ.get('CLT_PREFIX', '/opt/unixnu-linux/CommandLineTools/usr')
    env.update(PLATFORM='iPhoneOS', HOST_CC=f'{clt}/bin/xnu-host-cc', MIGCOM=f'{clt}/libexec/migcom')
    args += ['TIGHTBEAMC=tightbeamc-not-supported', 'BUILD_DSYM=0',
             f'MEMORY_SIZE={os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")}',
             f'SYSCTL_HW_PHYSICALCPU={os.cpu_count()}', f'SYSCTL_HW_LOGICALCPU={os.cpu_count()}',
             f'LD_KERNEL_LIBS={sdk}/usr/local/lib/kernel/libcc_kext.a']
(output / 'lto-command.json').write_text(json.dumps(args, indent=2) + '\n')
with (output / 'lto-build.log').open('w') as log:
    result = subprocess.run(args, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
if result.returncode == 0:
    # The OS profile names the reference next to the output dir; make writes it under the config dir.
    built = output / 'DEVELOPMENT_ARM64_BCM2837' / 'mach.development.bcm2837.unstripped.noctf'
    link = output / 'mach.release.bcm2837.unstripped.noctf'
    if built.is_file():
        link.unlink(missing_ok=True)
        link.symlink_to(built.relative_to(output))
print('RELEASE reference exit', result.returncode, 'log', output / 'lto-build.log', flush=True)
raise SystemExit(result.returncode)
