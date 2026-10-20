#!/usr/bin/env python3
"""Replay retained QEMU stack bytes through the native slice/encoding core; no VM run."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('observation', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
d = json.loads(args.observation.read_text())
selected = [p for p in d['ui_thread_snapshots']['processes'] if p['process']['name'] in ('backboardd', 'SpringBoard')]
assert len(selected) == 2
spec = importlib.util.spec_from_file_location('decoder', root/'scripts/decode-boot-ui-trace.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)
source = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "BootUiTraceCore.h"
struct pair {uint64_t fp,next,lr;};
static struct pair pairs[64];static unsigned pair_count;
static int read_pair(void *ctx,uint64_t address,void *bytes,unsigned size) {
 (void)ctx;assert(size==16);
 for(unsigned i=0;i<pair_count;++i)if(pairs[i].fp==address){uint64_t v[2]={pairs[i].next,pairs[i].lr};memcpy(bytes,v,16);return 0;}
 return 1;
}
int main(void){struct bt_groups groups={0};
'''
expected = []
for p in selected:
    role = 'B' if p['process']['name'] == 'backboardd' else 'S'
    assert len(p['threads']) <= 64
    for index, thread in enumerate(p['threads'], 1):
        if 'user_pc' not in thread:
            continue
        frames = thread.get('user_frames', [])
        pairs = [(int(f['fp'],16), int(frames[i+1]['fp'],16) if i+1<len(frames) else 0, int(f['lr'],16)) for i,f in enumerate(frames)]
        source += 'pair_count=%d;\n' % len(pairs)
        for i,(fp,nxt,lr) in enumerate(pairs):
            source += 'pairs[%d]=(struct pair){%dULL,%dULL,%dULL};\n' % (i,fp,nxt,lr)
        source += "{struct bt_entry e={.role='%s',.index=%d,.pc=%sULL,.lr=%sULL,.fp=%sULL};\n" % (role,index,thread['user_pc'],thread['user_lr'],thread['user_fp'])
        source += 'bt_walk(&e,read_pair,0);uint64_t deep[32]={0};struct bt_entry full=e;full.error=0;bt_walk_n(&full,read_pair,0,deep,32);assert(!full.error);\n'
        for n, frame in enumerate(frames[:32]):
            source += 'assert(deep[%d]==%sULL);\n' % (n, frame['lr'])
        source += 'char line[60];bt_format(&e,0,line);puts(line);bt_format(&e,1,line);puts(line);bt_add(&groups,&e);}\n'
        expected.append(dict(role=role,index=index,pc=thread['user_pc'],lr=thread['user_lr'],frames=[f['lr'] for f in frames[:8]]))
source += 'fprintf(stderr,"threads=%u groups=%u\\n",groups.threads,groups.count);return 0;}\n'
c = args.output/'replay.c';exe=args.output/'replay';c.write_text(source)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-I',str(root/'kits/uke'),str(c),'-o',str(exe)],check=True,timeout=45)
r=subprocess.run([str(exe)],capture_output=True,text=True,check=True,timeout=15)
(args.output/'encoded.txt').write_text(r.stdout)
rows=decoder.decode(r.stdout)
assert len(rows)==len(expected)
for actual, original in zip(rows,expected):
    for key in ('role','index','pc','lr'):
        assert actual[key] == original[key], (actual,original)
    frames=actual['frames_1_4']+actual['frames_5_8']
    assert frames[:len(original['frames'])]==original['frames'], (actual,original)
    assert not actual['error'], actual
(args.output/'report.json').write_text(json.dumps(dict(passed=True,observation_sha256=hashlib.sha256(args.observation.read_bytes()).hexdigest(),threads=len(rows),summary=r.stderr.strip(),source_stack_bytes_from_retained_QEMU=True,kernel_VM_API_not_executed=True,new_QEMU_run=False,hardware_root_cause_proven=False),indent=2)+'\n')
print('PASS: %d retained UI thread stacks replayed and decoded without losing PC/LR or first eight displayed returns and bounded 32-return walks; no new QEMU run' % len(rows))
