#!/usr/bin/env python3
"""Associate observed Metal writers, kernel-issued fences and display swaps."""
from argparse import ArgumentParser
from bisect import bisect_right
from collections import Counter, defaultdict
import json
from pathlib import Path
import runpy


def associate(gpu, frontend):
    producers = defaultdict(list)
    replies = defaultdict(list)
    waits = defaultdict(list)
    completions = {}
    for row in frontend:
        if row['name'] == 'scanout_producer':
            producers[row['pid'], row['runtime']].append(row)
        elif row['name'] == 'native_submit_fence':
            replies[row['pid'], row['runtime']].append(row)
        elif row['name'] == 'wait_scheduled':
            waits[row['pid'], row['thread']].append(row)
        elif row['name'] == 'command_summary' and row['completed_us']:
            completions[row['pid'], row['sequence']] = row['completed_us']
    tagged_waits = any(row['command'] and row['runtime']
                       for thread in waits.values() for row in thread)
    for thread in waits.values():
        thread.sort(key=lambda row: row['us'])
    wait_times = {key: [row['us'] for row in thread] for key, thread in waits.items()}
    submitted = {(row['pid'], row['fields']['fence']): row['us'] for row in gpu if row['event'] == 10}
    retired = {(row['pid'], row['fields']['fence']): row['us'] for row in gpu if row['event'] == 11}
    surfaces = {}
    for row in gpu:
        if row['event'] == 21:
            surfaces.setdefault(row['fields']['request'], row['fields'])

    writers = defaultdict(list)
    for key, rows in producers.items():
        generations = defaultdict(list)
        for row in rows:
            generations[row['command']].append(row)
        commands = sorted(generations.values(), key=lambda group: min(row['us'] for row in group))
        starts = [min(row['us'] for row in group) for group in commands]
        end_fences = defaultdict(list)
        for row in replies[key]:
            generation = bisect_right(starts, row['us']) - 1
            if generation >= 0:
                end_fences[generation].append(row)
        for generation, command in enumerate(commands):
            fences = end_fences[generation]
            for row in command:
                pid = row['pid']
                found = None
                if len(fences) == 1:
                    reply = fences[0]
                    native = submitted.get((pid, reply['fence']))
                    end = completions.get((pid, row['command']))
                    if (native is not None and row['us'] <= native <= reply['us']
                            and (end is None or reply['us'] <= end)):
                        found = dict(fence=reply['fence'], kernel_submit_us=native,
                                     retired_us=retired.get((pid, reply['fence'])))
                writers[pid, row['surface_id']].append(dict(**row, native=found))

    last_validation = {}
    for row in gpu:
        f = row['fields']
        if row['event'] != 20 or f['phase'] != 1 or not f['surface_id']:
            continue
        us, display, request = row['us'], row['display'], f['request']
        pid, sid = f['caller_pid'], f['surface_id']
        key = (display, pid, sid)
        previous = last_validation.get(key)
        last_validation[key] = us
        candidates = []
        handoff = None
        geometry = surfaces.get(request)
        if previous is None:
            status = 'first_seen'
        elif geometry is None:
            status = 'display_geometry_missing'
        else:
            candidates = [writer for writer in writers[pid, sid]
                          if previous < writer['us'] <= us and
                          all(writer[field] == geometry[field] for field in ('width', 'height', 'format'))]
            if len(candidates) != 1:
                status = 'ambiguous_writers' if candidates else 'writer_missing'
            elif not candidates[0]['native'] or candidates[0]['native']['kernel_submit_us'] > us:
                status = 'native_submission_unconfirmed'
            else:
                status = 'candidate'
                if tagged_waits:
                    writer = candidates[0]
                    thread_key = (pid, f['caller_thread'])
                    index = bisect_right(wait_times.get(thread_key, ()), us) - 1
                    if index >= 0:
                        wait = waits[thread_key][index]
                        if previous < wait['us'] and wait['us'] >= writer['native']['kernel_submit_us']:
                            handoff = wait
                    if not handoff:
                        status = 'handoff_missing'
                    elif all(handoff[field] == writer[field] for field in ('command', 'runtime')):
                        status = 'handoff_candidate'
                    else:
                        status = 'handoff_mismatch'
        yield dict(display=display, request=request, validated_us=us, surface_id=sid, pid=pid,
                   caller_thread=f['caller_thread'], status=status, writers=candidates, handoff=handoff)


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, help='export/files containing the GPU and frontend streams')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    decoder = runpy.run_path(str(Path(__file__).with_name('decode-gpu-trace.py')))
    _, gpu = decoder['records']((args.capture / 'gpu.trace.bin').read_bytes())
    frontend, pids, clock_offsets = [], set(), []
    for trace in sorted(args.capture.rglob('frames*.trace')):
        blob = trace.read_bytes()
        stride = decoder['FRAME_RECORD'].size
        rows = decoder['frontend_details'](blob[:len(blob) // stride * stride])
        headers = [row for row in rows if row['name'] == 'frame_segment']
        if len(headers) != 1:
            raise SystemExit(f'Expected a single identifiable frame segment in {trace}')
        header = headers[0]
        pids.add(header['pid'])
        clock_offsets.append(header['wall_us'] - header['us'])
        frontend.extend(dict(**row, pid=header['pid']) for row in rows
                        if row['name'] in ('scanout_producer', 'native_submit_fence',
                                           'wait_scheduled', 'command_summary'))
    if len(pids) != 1 or max(clock_offsets) - min(clock_offsets) > 1000:
        raise SystemExit('Frame streams must be from one producer and boot')
    args.out.mkdir(parents=True, exist_ok=True)
    counts = defaultdict(Counter)
    with (args.out / 'associations.jsonl').open('w') as stream:
        for row in associate(gpu, frontend):
            counts[row['display']][row['status']] += 1
            stream.write(json.dumps(row) + '\n')
    (args.out / 'summary.json').write_text(json.dumps(counts, indent=2) + '\n')


if __name__ == '__main__':
    main()
