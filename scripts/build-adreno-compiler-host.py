#!/usr/bin/env python3
"""Build pinned host IR3/NIR compiler tools; never install a rendering driver."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--probe', action='store_true', help='Compile and roundtrip the source-bound A740 synthetic vertex fixture')
args = parser.parse_args()
ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache'
OUT = ROOT / 'build/mesa-ir3'
pins = json.loads((ROOT / 'scripts/data/adreno-compiler-inputs.json').read_text())
source = CACHE / 'mesa-work'
venv = CACHE / 'mesa-venv'
tools = CACHE / 'mesa-tools'


def run(command, *, cwd=ROOT, log=None, timeout=60, env=None):
    command = [str(x) for x in command]
    if log:
        with (CACHE / log).open('w') as stream:
            result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        if result.returncode:
            raise RuntimeError(log + ':\n' + '\n'.join((CACHE / log).read_text().splitlines()[-20:]))
        return ''
    return subprocess.check_output(command, cwd=cwd, env=env, text=True, timeout=timeout).strip()


CACHE.mkdir(exist_ok=True)
if not (source / '.git').exists():
    run(['git', 'init', source])
    run(['git', '-C', source, 'remote', 'add', 'origin', pins['mesa']['mirror']])
    run(['git', '-C', source, 'fetch', '--depth=1', '--filter=blob:none', 'origin', pins['mesa']['commit']], timeout=180)
    run(['git', '-C', source, 'sparse-checkout', 'init', '--cone'])
    run(['git', '-C', source, 'sparse-checkout', 'set', *pins['mesa']['sparse_directories']])
    run(['git', '-C', source, 'checkout', '--detach', 'FETCH_HEAD'], timeout=180)
if run(['git', '-C', source, 'rev-parse', 'HEAD']) != pins['mesa']['commit'] or \
        run(['git', '-C', source, 'rev-parse', 'HEAD^{tree}']) != pins['mesa']['tree']:
    raise SystemExit('Pinned Mesa commit/tree differs')
for name in pins['patches']:
    patch = ROOT / name
    applied = subprocess.run(['git', '-C', source, 'apply', '--check', '--reverse', patch], capture_output=True)
    if applied.returncode:
        run(['git', '-C', source, 'apply', '--check', patch])
        run(['git', '-C', source, 'apply', patch])
# The working tree must be exactly the recorded patches: reverse them all (newest first) and require a clean tree, then
# re-apply them in order. A comparison of concatenated per-patch diffs would break once two patches touch one file.
def working_tree_is_patches():
    done = []
    try:
        for name in reversed(pins['patches']):
            run(['git', '-C', source, 'apply', '--reverse', ROOT / name])
            done.append(name)
        return subprocess.run(['git', '-C', source, 'diff', '--quiet'], capture_output=True).returncode == 0
    except subprocess.CalledProcessError:
        return False
    finally:
        for name in reversed(done):
            run(['git', '-C', source, 'apply', ROOT / name])
if not working_tree_is_patches():
    raise SystemExit('Mesa working changes must match the exact recorded patches')

if not (venv / 'bin/python').exists():
    run([sys.executable, '-m', 'venv', venv])
requirements = (ROOT / 'scripts/requirements-adreno-compiler.txt').read_text().splitlines()
expected_packages = {line.split('==')[0].lower(): line.split('==')[1] for line in requirements if line.strip()}
installed = json.loads(run([venv / 'bin/python', '-c', 'import importlib.metadata,json; print(json.dumps({d.metadata["Name"].lower():d.version for d in importlib.metadata.distributions()}))']))
if any(installed.get(name) != version for name, version in expected_packages.items()):
    run([venv / 'bin/pip', 'install', '-r', ROOT / 'scripts/requirements-adreno-compiler.txt'],
        log='mesa-python-install.log', timeout=180)

bison = tools / 'bin/bison'
if not bison.exists() or run([bison, '--version']).splitlines()[0] != 'bison (GNU Bison) 3.8.2':
    archive = CACHE / 'tool-sources/bison-3.8.2.tar.xz'
    archive.parent.mkdir(exist_ok=True)
    if not archive.exists():
        with urllib.request.urlopen(pins['bison']['url'], timeout=30) as stream:
            archive.write_bytes(stream.read(pins['bison']['bytes'] + 1))
    data = archive.read_bytes()
    if len(data) != pins['bison']['bytes'] or hashlib.sha256(data).hexdigest() != pins['bison']['sha256']:
        raise SystemExit('GNU Bison release input differs')
    tool_source = archive.parent / 'bison-3.8.2'
    if not tool_source.exists():
        with tarfile.open(archive) as stream:
            stream.extractall(archive.parent, filter='data')
    tool_build = CACHE / 'tool-build/bison'
    tool_build.mkdir(parents=True, exist_ok=True)
    run([tool_source / 'configure', '--prefix=' + str(tools), '--disable-nls'], cwd=tool_build,
        log='bison-configure.log', timeout=300)
    run(['make', '-j4'], cwd=tool_build, log='bison-build.log', timeout=300)
    run(['make', 'install'], cwd=tool_build, log='bison-install.log', timeout=60)

clean_env = {k: v for k, v in os.environ.items() if not k.startswith(('IR3_', 'FD_MESA_', 'NIR_', 'MESA_SHADER_'))}
env = dict(clean_env, PATH=str(tools / 'bin') + ':' + str(venv / 'bin') + ':' + os.environ['PATH'],
           CC='/usr/bin/clang', CXX='/usr/bin/clang++', CFLAGS='', CPPFLAGS='', CXXFLAGS='', LDFLAGS='')
options = ['-Dgallium-drivers=', '-Dvulkan-drivers=', '-Dplatforms=', '-Dglx=disabled', '-Degl=disabled',
           '-Dgbm=disabled', '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled', '-Dllvm=disabled',
           '-Dshared-glapi=disabled', '-Dxmlconfig=disabled', '-Dshader-cache=disabled',
           '-Dtools=freedreno', '-Dbuild-tests=true', '-Dvideo-codecs=']
if args.probe:
    options += ['-Doffline-adreno-probe=' + str(ROOT / 'kits/uke/AdrenoCompilerProbe.c')]
if not (OUT / 'build.ninja').exists():
    run([venv / 'bin/meson', 'setup', OUT, source, '--wrap-mode=nodownload', *options],
        log='mesa-ir3-configure.log', env=env, timeout=180)
elif args.probe:
    run([venv / 'bin/meson', 'configure', OUT, '-Doffline-adreno-probe=' + str(ROOT / 'kits/uke/AdrenoCompilerProbe.c')],
        log='mesa-ir3-probe-configure.log', env=env, timeout=30)
configuration = {v['name']: v['value'] for v in json.loads((OUT / 'meson-info/intro-buildoptions.json').read_text())}
for name, value in {'gallium-drivers': [], 'vulkan-drivers': [], 'platforms': [], 'opengl': False,
                    'glx': 'disabled', 'egl': 'disabled', 'gbm': 'disabled', 'llvm': 'disabled',
                    'gles1': 'disabled', 'gles2': 'disabled', 'shader-cache': 'disabled',
                    'xmlconfig': 'disabled', 'wrap_mode': 'nodownload', 'tools': ['freedreno']}.items():
    if configuration.get(name) != value:
        raise SystemExit('Compiler-only build option differs: ' + name)
targets = ['src/freedreno/ir3/ir3_disasm', 'src/freedreno/ir3/ir3_delay_test']
if args.probe:
    targets += ['src/freedreno/ir3/adreno_compiler_probe']
run(['ninja', '-C', OUT, '-j4', *targets],
    log='mesa-ir3-build.log', env=env, timeout=300)
results = {}
for name in ('ir3_disasm', 'ir3_delay_test'):
    binary = OUT / 'src/freedreno/ir3' / name
    stdout = run([binary], env=env, timeout=30)
    (CACHE / ('mesa-' + name + '-test.log')).write_text(stdout + '\n')
    if name == 'ir3_disasm' and not stdout.endswith('PASSED!'):
        raise SystemExit('Original ISA-vector test did not report real success')
    results[name] = dict(binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                         output_sha256=hashlib.sha256(stdout.encode()).hexdigest(), exit_code=0)
    if name == 'ir3_disasm':
        devices = re.findall(r'^Testing (.*?) [0-9a-f]{8}_', stdout, re.M)
        results[name]['cases'] = len(devices)
        results[name]['per_source_device'] = {device: devices.count(device) for device in sorted(set(devices))}
    else:
        results[name]['cases'] = len(re.findall(r'^\d+: PASS$', stdout, re.M))

if results['ir3_disasm']['per_source_device'] != {'FD630': 285, 'FD730': 53, 'Adreno (TM) 830': 6, 'FD540': 22, 'FD420': 2} or results['ir3_delay_test']['cases'] != 12:
    raise SystemExit('Exact upstream test coverage differs; do not report a filtered/smaller run as complete')

probe = None
if args.probe:
    artifact = ROOT / 'build/adreno-compiler-probe/vertex'
    artifact.parent.mkdir(parents=True, exist_ok=True)
    executable = OUT / 'src/freedreno/ir3/adreno_compiler_probe'
    probe_env = dict(env, IR3_SHADER_DEBUG='asmroundtrip')
    probe = json.loads(run([executable, artifact], env=probe_env, timeout=30))
    binary, assembly, nir = (artifact.with_suffix(ext).read_bytes() for ext in ('.bin', '.asm', '.nir'))
    if (probe['chip_id'] != '0x43050a01' or probe['name'] != 'FD740' or probe['generation'] != 7 or
            probe['binary_bytes'] != len(binary) or probe['code_dwords'] != 32 or len(binary) != 128 or
            hashlib.sha256(binary).hexdigest() != 'fc31f2ba6bfa38087029ddd99df1019a168c15c23195d89b9f7534bf827c65a3' or
            not probe['unknown_chip_refused'] or not probe['synthetic_nir'] or not probe['asm_roundtrip'] or probe['gpu_executed'] or
            b'cov.u32f32' not in assembly or b'add.f' not in assembly or b'SYSTEM_VALUE_VERTEX_ID' not in assembly or
            b'VARYING_SLOT_POS' not in assembly or b'@load_vertex_id' not in nir):
        raise SystemExit('Actual A740 compiler output or roundtrip evidence differs')
    # Refuse a run that would silently omit the requested compiler check.
    negative = subprocess.run([executable, artifact.parent / 'no-roundtrip'], env=dict(env, IR3_SHADER_DEBUG=''),
                              capture_output=True, timeout=30)
    if negative.returncode != 5:
        raise SystemExit('Missing roundtrip option must genuinely refuse before output')
    probe.update(source_sha256=hashlib.sha256((ROOT / 'kits/uke/AdrenoCompilerProbe.c').read_bytes()).hexdigest(),
                 executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                 binary_sha256=hashlib.sha256(binary).hexdigest(), assembly_sha256=hashlib.sha256(assembly).hexdigest(),
                 nir_sha256=hashlib.sha256(nir).hexdigest(), missing_roundtrip_refused=True)

report = dict(mesa_commit=pins['mesa']['commit'], mesa_tree=pins['mesa']['tree'],
              patches={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in pins['patches']},
              python_packages=expected_packages, bison_source_sha256=pins['bison']['sha256'],
              bison_binary_sha256=hashlib.sha256(bison.read_bytes()).hexdigest(),
              compiler_only_options=options, upstream_tests=results,
              host_only=True, a740_synthetic_compile=probe, ios_compiler_cross_build_qualified=False,
              original_air_compiled=False, gpu_execution_qualified=False, metal_qualified=False)
(OUT / 'host-report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: pinned compiler-only IR3/NIR host build; 368 ISA vectors and 12 A630 delay cases')
if probe:
    print('PASS: actual A740 synthetic vertex code: 128 bytes, internal asm roundtrip, unknown-chip lookup and missing-roundtrip refusals')
print('No rendering driver or deployment guest; A740 shader execution and AIR compilation remain unqualified')
