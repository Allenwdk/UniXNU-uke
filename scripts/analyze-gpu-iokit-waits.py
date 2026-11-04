#!/usr/bin/env python3
"""Join the optional GPU IOKit phases in decoded latency.jsonl. No device access."""
import argparse
import collections
import json
from pathlib import Path


def analyze(rows):
    active, complete, orphan = {}, [], 0
    for row in sorted(rows, key=lambda r: r['us']):
        if row['name'] not in ('gpu_iokit_external', 'gpu_iokit_map'):
            continue
        f = row['fields']
        key = (row['name'], row['pid'], row['thread'], f['client'])
        phase = f['phase']
        if phase == 1:
            if key in active:
                orphan += 1
            active[key] = dict(kind=row['name'], pid=row['pid'], thread=row['thread'], client=f['client'],
                               token=f['token'], locking=f['locking'], begin_us=row['us'], phases={1:f})
        elif key in active:
            call = active[key]
            call['phases'][phase] = f
            if phase == 2:
                call['locked_us'] = row['us']
            if phase == 3:
                active.pop(key)
                call.update(end_us=row['us'], elapsed_us=f['elapsed_us'], status=f['status'])
                acquired = call['phases'].get(2)
                body = call['phases'].get(4)
                call['lock_wait_us'] = acquired['elapsed_us'] if acquired else None
                call['map_body_us'] = body['elapsed_us']-acquired['elapsed_us'] if body and acquired else None
                complete.append(call)
        else:
            orphan += 1
    external = [c for c in complete if c['kind']=='gpu_iokit_external']
    maps = [c for c in complete if c['kind']=='gpu_iokit_map']
    slow = []
    for m in maps:
        if m['elapsed_us'] < 1000:
            continue
        # Same client, different thread, while this map is acquiring the write lock.
        # Overlap is evidence to inspect, not automatic proof that this call held it.
        candidates = [e for e in external if e['client']==m['client'] and e['thread']!=m['thread']
                      and e.get('locked_us',e['end_us'])<m.get('locked_us',m['begin_us'])
                      and e['end_us']>m['begin_us'] and e['locking']&1]
        slow.append(dict(m, overlapping_external_calls=candidates[:32], overlaps_omitted=max(0,len(candidates)-32)))
    def summary(calls):
        return dict(count=len(calls), over_1ms=sum(c['elapsed_us']>=1000 for c in calls),
                    max_us=max((c['elapsed_us'] for c in calls),default=0),
                    max_lock_wait_us=max((c['lock_wait_us'] or 0 for c in calls),default=0))
    return dict(external=summary(external), maps=summary(maps),
                slow_maps=sorted(slow,key=lambda c:c['elapsed_us'],reverse=True),
                incomplete=list(active.values()), orphan_phases=orphan,
                notes=['Times are uptime microseconds. IOKit token is memory_type or outer selector (possibly a tunnel).',
                       'Lock flags: 1 default locking; 2 single-thread external. External lock is read unless bit2 is set; map lock is write.',
                       'Phase 1 precedes lock acquisition, 2 follows it, 4 follows mapClientMemory64, 3 follows IPC exit.',
                       'Slow maps and overlapping external calls require inspection; overlap alone does not establish causality.'])


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('latency',type=Path)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    result=analyze([json.loads(line) for line in args.latency.read_text().splitlines()])
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ('external','maps','orphan_phases')}))
