#!/usr/bin/env python3
"""Execute the real panic renderer in bounded QEMU, with fixture memory only.

Uses the candidate's actual high virtual symbol addresses and the same compiled
renderer bytes. This is not an XNU boot or a hardware-memory observation.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--candidate', type=Path, default=ROOT / 'build/native-prelink/native-owner-ufs-rootauth3-candidate.macho')
p.add_argument('--renderer-source', type=Path, default=ROOT / 'scripts/xnu-panic-screen.c', help='Renderer under test; permits old/new regression controls')
p.add_argument('--message-file', type=Path, help='Bounded ASCII diagnostic fixture; no printf conversions')
p.add_argument('--output', type=Path, default=ROOT / 'build/panic-screen-qemu')
p.add_argument('--frame-chain', action='store_true', help='Six bounded high-address frame links, including raw PAC bits')
p.add_argument('--competing-panic', action='store_true', help='Two CPUs race distinct panic messages; only one may paint')
p.add_argument('--exec-policy-format', action='store_true', help='Heap strings, precision, unmapped page and unsafe-pointer refusal')
args = p.parse_args()
message = args.message_file.read_text().rstrip('\n') if args.message_file else 'PANIC SCREEN OFFLINE'
heap = 0xfffffff4d0010000
if args.competing_panic:
    message = 'PANIC CPU0 WINS'
if args.exec_policy_format:
    message = ('unexpected SIGKILL of launchd  with reason -- namespace 9 code 0x8 description none\n'
               'precision=hel zero= negative=launchd\n'
               'guard=TAIL<fffffff4d0011000> unsafe=<40000000> absent=<fffffff600000000> device=<fffffff5c0000000>')
if len(message) >= 2500 or '%' in message or any(c != '\n' and not 32 <= ord(c) <= 126 for c in message):
    p.error('Diagnostic fixture must be printable ASCII, under 2500 bytes, without printf conversions')
OUT = args.output.resolve()
OUT.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('probe', ROOT / 'scripts/patch-xnu-entry-probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
candidate = args.candidate
data = candidate.read_bytes()
probe.load_segments(data)
names = probe.rebase(candidate, data)
code, load, entry = probe.assemble_c(args.renderer_source,
                                   probe.CAVES[2] + probe.CAVE_SIZE, OUT, names)
assert load + len(code) <= probe.PANIC_CAVE_END


def physical(va):
    # Fixture-only 1 GiB block alias for the candidate's high virtual addresses.
    assert va >> 30 == 0xfffffff000000000 >> 30
    return 0x40000000 + (va & 0x3fffffff)


width, height = (3048, 2032) if (args.message_file or args.exec_policy_format or args.frame_chain) else (800, 900)
rowbytes = width * 4
assert max(map(len, message.split('\n'))) * 18 <= width
assert height // 8 + 8 + (len(message.split('\n')) + 10) * 42 < height
frame = 0x42000000
vinfo = struct.pack('<IIIIQ', height, width, 32, rowbytes, 0xfffffff002000000)
(OUT / 'vinfo.bin').write_bytes(vinfo)
# Fill the whole panel, including row zero, with old log pixels.
(OUT / 'old-framebuffer.bin').write_bytes(bytes.fromhex('44332211') * (width * height))
# Match the actual UFS panic("%s", diagnostic) call, including Darwin's
# stack-passed variadic argument and the renderer's readable-string check.
(OUT / 'format.bin').write_bytes(b'%s\0')
(OUT / 'message.bin').write_bytes(message.encode('ascii') + b'\0')
if args.competing_panic:
    (OUT / 'message-cpu1.bin').write_bytes(b'PANIC CPU1 WINS\0')
if args.exec_policy_format:
    (OUT / 'format.bin').write_bytes(b'unexpected SIGKILL of %s %s with reason -- namespace %d code 0x%llx description %.800s\nprecision=%.*s zero=%.0s negative=%.*s\nguard=%s unsafe=%s absent=%s device=%s\0')
    page=bytearray(4096)
    page[:8]=b'launchd\0';page[32:37]=b'none\0';page[64:70]=b'hello\0';page[-4:]=b'TAIL'
    (OUT / 'message.bin').write_bytes(page)
heap_setup = ''
argument_setup = '  ldr x2, =0xfffffff004010000\n  str x2, [sp]'
if args.exec_policy_format:
    # Native 4K leaf for an out-of-image heap string, followed by an unmapped
    # guard. All addresses here belong only to this QEMU fixture.
    heap_setup = f'''  ldr x0, ={0x40201000+((0xfffffff5c0000000>>30)&255)*8:#x}
  ldr x1, =0x40000705
  str x1, [x0]
  ldr x0, ={0x40201000+((heap>>30)&255)*8:#x}
  ldr x1, =0x40202003
  str x1, [x0]
  ldr x0, ={0x40202000+((heap>>21)&511)*8:#x}
  ldr x1, =0x40203003
  str x1, [x0]
  ldr x0, ={0x40203000+((heap>>12)&511)*8:#x}
  ldr x1, =0x44010403
  str x1, [x0]
'''
    values = [heap,heap+7,9,8,heap+32,3,heap+64,0,0xffffffff,heap,heap+4092,0x40000000,0xfffffff600000000,0xfffffff5c0000000]
    argument_setup='\n'.join(f'  ldr x2, ={v:#x}\n  str x2, [sp, #{i*8}]' for i,v in enumerate(values))
# Poison the high half adjacent to the u32 counters to check the read widths.
(OUT / 'max-mem.bin').write_bytes(struct.pack('<Q', 0x40000000))
(OUT / 'total.bin').write_bytes(struct.pack('<II', 0x12345, 0xdeadbeef))
(OUT / 'free.bin').write_bytes(struct.pack('<II', 0xabcd, 0xdeadbeef))
frame_setup = '  mov x29, #0\n  mov x30, #0\n'
expected_trace = ''
if args.frame_chain:
    assert not args.competing_panic and not args.exec_policy_format
    stack = 0xfffffff001100000
    links = b''.join(struct.pack('<QQ', stack+0x2000+(n+1)*16 if n<7 else 0,
                                 0xabcdfff00b002000+n*4) for n in range(8))
    (OUT / 'frame-chain.bin').write_bytes(links)
    frame_setup = '  ldr x29, =0xfffffff001102000\n  ldr x30, =0xabcdfff00b001000\n'
    expected_trace = '\nBT 0xabcdfff00b001000' + ''.join(' '+hex(0xabcdfff00b002000+n*4) for n in range(5))
race_setup = ''
race_entry = ''
if args.competing_panic:
    assert not args.exec_policy_format
    assert '_uke_panic_shadow_len' in names
    race_setup = """  ldr x2, =secondary_start
  mov x0, #3
  movk x0, #0xc400, lsl #16
  mov x1, #1
  mov x3, #0
  hvc #0
  mov x3, #0
  b race_ready
secondary_start:
  msr daifset, #15
  ldr x0, =0x40200000
  msr ttbr0_el1, x0
  ldr x0, =0x40201000
  msr ttbr1_el1, x0
  mov x0, #0x44
  msr mair_el1, x0
  ldr x0, =0x22a51a251a
  msr tcr_el1, x0
  dsb sy
  isb
  ldr x0, =0x3454593d
  msr sctlr_el1, x0
  isb
  ldr x0, =0x41200000
  mov sp, x0
  stp xzr, xzr, [sp]
  stp xzr, xzr, [sp, #16]
  stp xzr, xzr, [sp, #32]
  ldr x2, =0xfffffff004020000
  str x2, [sp]
  mov x3, #1
race_ready:
  ldr x4, =0x40204000
1:
  ldaxr w5, [x4]
  add w5, w5, #1
  stlxr w6, w5, [x4]
  cbnz w6, 1b
2:
  ldar w5, [x4]
  cmp w5, #2
  b.lo 2b
"""
asm = f'''.text
.global _start
_start:
  msr daifset, #15
  ldr x0, =0x40200000
  mov x1, #1024
1:
  stp xzr, xzr, [x0], #16
  subs x1, x1, #1
  b.ne 1b
  ldr x0, =0x40200008
  ldr x1, =0x40000701
  str x1, [x0]
  ldr x0, =0x40201600
  str x1, [x0]
{heap_setup}  ldr x0, =0x40200000
  msr ttbr0_el1, x0
  ldr x0, =0x40201000
  msr ttbr1_el1, x0
  mov x0, #0x44
  msr mair_el1, x0
  ldr x0, =0x22a51a251a
  msr tcr_el1, x0
  dsb sy
  isb
  tlbi vmalle1
  dsb sy
  isb
  ldr x0, =0x3454593d
  msr sctlr_el1, x0
  isb
  ldr x0, ={0xfffffff001100000 if args.frame_chain else 0x41100000:#x}
  mov sp, x0
  stp xzr, xzr, [sp]
  stp xzr, xzr, [sp, #16]
  stp xzr, xzr, [sp, #32]
{argument_setup}
{frame_setup}{race_setup}  ldr x0, =0xfffffff004000000
  ldr x1, ={entry:#x}
  br x1
'''
(OUT / 'entry.S').write_text(asm)
subprocess.run(['clang', '--target=aarch64-none-elf', '-c', OUT / 'entry.S', '-o', OUT / 'entry.o'], check=True)
subprocess.run(['ld.lld', '-Ttext=0x40080000', '-e', '_start', OUT / 'entry.o', '-o', OUT / 'entry.elf'], check=True)
subprocess.run(['llvm-objcopy', '-O', 'binary', OUT / 'entry.elf', OUT / 'entry.bin'], check=True)
command = ['qemu-system-aarch64', '-M', 'virt,virtualization=off,secure=off',
           '-accel', 'tcg', '-cpu', 'cortex-a710', '-m', '512M', '-smp', '2' if args.competing_panic else '1',
           '-display', 'none', '-serial', 'none', '-monitor', 'none', '-nic', 'none',
           '-no-reboot', '-kernel', str(OUT / 'entry.bin')]
for file, address in (
    ('panic.bin', physical(load)), ('vinfo.bin', physical(probe.VINFO_VA)),
    ('old-framebuffer.bin', frame),
    ('format.bin', 0x44000000), ('message.bin', 0x44010000), ('max-mem.bin', physical(names['_max_mem'])),
    ('total.bin', physical(names['_vm_page_pages'])),
    ('free.bin', physical(names['_vm_page_free_count'])),
):
    command += ['-device', f'loader,file={OUT / file},addr={address:#x},force-raw=on']
if args.frame_chain:
    command += ['-device', f'loader,file={OUT / "frame-chain.bin"},addr=0x41102000,force-raw=on']
if args.competing_panic:
    command += ['-device', f'loader,file={OUT / "message-cpu1.bin"},addr=0x44020000,force-raw=on']
sockpath = OUT / 'qmp.sock'
sockpath.unlink(missing_ok=True)
command += ['-qmp', f'unix:{sockpath},server=on,wait=off']

with (OUT / 'stderr.log').open('w') as errors:
    process = subprocess.Popen(command, stdout=errors, stderr=errors)
    try:
        deadline = time.monotonic() + 5
        while not sockpath.exists() and time.monotonic() < deadline and process.poll() is None:
            time.sleep(0.02)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2)
            sock.connect(str(sockpath))
            stream = sock.makefile('rwb')
            json.loads(stream.readline())

            def qmp(name, arguments=None):
                request = {'execute': name}
                if arguments:
                    request['arguments'] = arguments
                stream.write(json.dumps(request).encode() + b'\n')
                stream.flush()
                while True:
                    result = json.loads(stream.readline())
                    if 'error' in result:
                        raise RuntimeError(result)
                    if 'return' in result:
                        return result['return']

            qmp('qmp_capabilities')
            marker = OUT / 'marker.bin'
            while time.monotonic() < deadline:
                qmp('pmemsave', {'val': frame + (height - 1) * rowbytes,
                                'size': 8, 'filename': str(marker)})
                if marker.read_bytes() == struct.pack('<II', 0xc0ffee11, 0x5ceeda7a):
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError('Renderer did not finish within five seconds')
            qmp('stop')
            qmp('pmemsave', {'val': frame, 'size': rowbytes * height,
                            'filename': str(OUT / 'framebuffer.bin')})
            if '_boot_progress_panic_text' in names:
                qmp('pmemsave', {'val': physical(names['_boot_progress_panic_text']), 'size': 256,
                                'filename': str(OUT / 'panic-text.bin')})
            if args.competing_panic:
                qmp('pmemsave', {'val': physical(names['_uke_panic_shadow']), 'size': 1024, 'filename': str(OUT / 'winner.bin')})
                qmp('pmemsave', {'val': physical(names['_uke_panic_shadow_len']), 'size': 4, 'filename': str(OUT / 'owner-length.bin')})
            registers = qmp('human-monitor-command', {'command-line': 'info registers'})
            (OUT / 'registers.txt').write_text(registers)
    finally:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        sockpath.unlink(missing_ok=True)

syms = subprocess.check_output(['llvm-nm', OUT / 'panic.elf'], text=True)
font = next(int(line.split()[0], 16) for line in syms.splitlines() if line.endswith(' gFont'))
glyphs = struct.unpack_from('<95Q', code, font - load)
pixels = (OUT / 'framebuffer.bin').read_bytes()
if args.competing_panic:
    message = (OUT / 'winner.bin').read_bytes().split(b'\0', 1)[0].decode('ascii')
    assert message in ('PANIC CPU0 WINS', 'PANIC CPU1 WINS'), message
    assert struct.unpack('<I', (OUT / 'owner-length.bin').read_bytes())[0] == len(message)
lines = (message + expected_trace).split('\n')
expected_pixels = bytearray(rowbytes * height)
for line, text in enumerate(lines):
    for column, char in enumerate(text):
        glyph = glyphs[ord(char) - 32]
        for y in range(42):
            row = y // 3
            bits = (glyph >> 32) if row < 6 else (glyph & 0xffffffff)
            bits >>= ((row if row < 6 else row - 6) * 5)
            for x in range(18):
                on = row < 12 and x // 3 < 5 and bits & (1 << (x // 3))
                at = (line * 42 + y) * rowbytes + (column * 18 + x) * 4
                actual = struct.unpack_from('<I', pixels, at)[0]
                expected = 0xffffffff if on else 0
                assert actual == expected, (line, column, x, y)
                struct.pack_into('<I', expected_pixels, at, expected)
struct.pack_into('<II', expected_pixels, (height - 1) * rowbytes, 0xc0ffee11, 0x5ceeda7a)
assert pixels == expected_pixels, 'Old framebuffer pixels remain outside the message'
# The board's banner copy of the message (kits/uke/BootProgress.c): the formatted text only, newlines as spaces, NUL-terminated.
copied = None
if '_boot_progress_panic_text' in names:
    raw = (OUT / 'panic-text.bin').read_bytes()
    copied = raw[:raw.index(0)].decode('ascii')
    assert copied == message.replace('\n', ' ')[:255], copied
report = {'panic_text_copy': copied, 'evidence': 'QEMU TCG execution of diagnostic only; synthetic memory counters',
          'renderer_sha256': hashlib.sha256(code).hexdigest(), 'renderer_bytes': len(code),
          'renderer_va': hex(load), 'matched_lines': lines, 'full_panel_old_pixels_cleared': True, 'two_cpu_panic_race': args.competing_panic, 'bounded_high_va_frame_chain': args.frame_chain,
          'hardware_or_xnu_boot_qualified': False}
(OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'PASS: actual ARM64 panic renderer clears the full panel and draws all {len(lines)} expected lines')
if copied is not None:
    print(f'PASS: the renderer copied the formatted message ({len(copied)} bytes) into _boot_progress_panic_text')
print('QEMU fixture only; no XNU boot, actual memory capacity or hardware fault cause established')
