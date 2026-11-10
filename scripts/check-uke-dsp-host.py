#!/usr/bin/env python3
"""Check actual Uke battery/SoC runtime code with sanitizers and modeled MMIO."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

DEVICE = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--soc', type=Path, default=DEVICE.parent / 'UniXNU-sm7675-common')
args = parser.parse_args()
remote = args.soc.resolve() / 'kits/Sm7675Platform/remoteproc'
flags = ['-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined']
with tempfile.TemporaryDirectory(prefix='uke-dsp-host-') as directory:
    scratch = Path(directory)
    exe = scratch / 'battery'
    subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', *flags,
                    DEVICE / 'package/tests/uke_battmgr_host.c', DEVICE / 'package/ShengBattmgr.c',
                    '-o', exe], check=True)
    subprocess.run([exe], check=True)
    source = (DEVICE / 'package/ShengBattery.cpp').read_text()
    start = source.index('        if (__atomic_exchange_n(&self->channelChanged')
    end = source.index('        for (;;) {', start)
    fixture = (DEVICE / 'package/tests/uke_battery_state_host.cpp').read_text()
    fixture = fixture.replace('#include "../ShengBattmgr.h"', '#include "ShengBattmgr.h"')
    state_source = scratch / 'battery-state.cpp'
    state_source.write_text(fixture + '\nvoid productionState(ShengBattery *self) {\n' + source[start:end] + '}\n')
    battery_object = scratch / 'battmgr.o'
    subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', *flags, '-c',
                    DEVICE / 'package/ShengBattmgr.c', '-o', battery_object], check=True)
    state_exe = scratch / 'battery-state'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', *flags,
                    '-I', DEVICE / 'package', state_source, battery_object, '-o', state_exe], check=True)
    subprocess.run([state_exe], check=True)
    refresh_exe = scratch / 'battery-refresh'
    subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', *flags,
                    DEVICE / 'package/tests/uke_battmgr_refresh_host.c', battery_object,
                    '-o', refresh_exe], check=True)
    subprocess.run([refresh_exe], check=True)
    start = source.index('    static void dspState(')
    end = source.index('    void subscribe()', start)
    fixture = (DEVICE / 'package/tests/uke_battery_dispatch_host.cpp').read_text()
    dispatch_source = scratch / 'battery-dispatch.cpp'
    dispatch_source.write_text(fixture.replace('    // PRODUCTION_CALLBACKS', source[start:end]))
    dispatch_exe = scratch / 'battery-dispatch'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', *flags,
                    dispatch_source, '-o', dispatch_exe], check=True)
    subprocess.run([dispatch_exe], check=True)
    # The real shared runtime is included by the fixture. These empty headers
    # replace kernel declarations supplied by its modeled MMIO/lock fixture.
    for name in ('IOKit/IODeviceMemory.h', 'IOKit/IOLib.h', 'kern/locks.h', 'kern/thread.h'):
        header = scratch / name
        header.parent.mkdir(exist_ok=True)
        header.touch()
    objects = []
    for name in ('QcomQmp', 'QcomRpmh', 'QcomSmem'):
        obj = scratch / (name + '.o')
        subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', *flags, '-c',
                        remote / (name + '.c'), '-o', obj], check=True)
        objects.append(obj)
    exe = scratch / 'remoteproc'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', *flags,
                    '-Wno-ignored-attributes', '-Wno-pointer-bool-conversion',
                    '-DSM7675_REMOTEPROC_HOST', '-I', scratch,
                    remote / 'tests/remoteproc_host.cpp', *objects, '-o', exe], check=True)
    subprocess.run([exe], check=True)
    subprocess.run([exe, 'v3'], check=True)
