#!/usr/bin/env python3
"""Build the opt-in pram log writer against this source kernel's actual ABI."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/uke-last-log'
OUT.mkdir(parents=True, exist_ok=True)

STARTUP_RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
KERN_EXEC_RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json'


def compile_source(source, record_name, obj):
    record = json.loads((ROOT / record_name).read_text().strip().rstrip(','))
    command = []
    args = record['arguments']
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == record['file']:
            command.append(str(source))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                command += ['-o', str(obj)]
            i += 1
        elif arg == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not arg.startswith('-W'):
            command.append(arg)
        i += 1
    command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror']
    subprocess.run(command, cwd=record['directory'], check=True, timeout=120)
    (OUT / (obj.stem + '-compile-command.json')).write_text(json.dumps(command, indent=2) + '\n')
    return hashlib.sha256(source.read_bytes()).hexdigest(), hashlib.sha256(obj.read_bytes()).hexdigest()


sources = [
    ('UkeLastLog.c', STARTUP_RECORD, 'UkeLastLog.o'),
    ('BootWatchdogPolicy.c', KERN_EXEC_RECORD, 'BootWatchdogPolicy.o'),
    ('CrashLogProbe.c', KERN_EXEC_RECORD, 'CrashLogProbe.o'),
]
report = {}
objects = []
for name, record, obj_name in sources:
    source = ROOT / 'kits/uke' / name
    obj = OUT / obj_name
    source_sha, object_sha = compile_source(source, record, obj)
    report[name] = {'source_sha256': source_sha, 'object_sha256': object_sha}
    objects.append(obj)

archive = OUT / 'libUkeLastLog.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True, timeout=30)
report['archive_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
report['hardware_execution'] = False
(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: strict source-kernel ARM64 pram writer, watchdog policy and crash probe build')
