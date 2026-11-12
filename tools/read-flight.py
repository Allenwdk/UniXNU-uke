#!/usr/bin/env python3
"""Read the continuous Uke FUA journal; keep each boot and its gaps separate."""
import argparse
import collections
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import uuid
import zlib

BLOCK=4096
REGION_BYTES=16*1024*1024
SLOT=65536
HEADER=struct.Struct('<8sII4Q40s4I24s')
PIECE=struct.Struct('<IIQ')
BEAT_NAMES=('us','tick','gpu_cursor','critical_cursor','normal_cursor','gpu_lost','critical_lost','normal_lost','write_errors',
 'gpu_submit','gpu_fence','display_offer','display_flip','display_latch','display_retire','data_read','data_write','data_flush','data_errors',
 'gpu_hang','gpu_fault','gpu_recovery','gpu_level','gpu_busy','kernel_reference','probe_passes','probe_started_us','probe_done_us','probe_phase','probe_pid','probe_tid','reserved')


def scan(blob):
    if len(blob)!=REGION_BYTES:raise ValueError('Expected the complete 16 MiB flight journal')
    groups=collections.defaultdict(list);rejected=collections.Counter()
    for index in range(REGION_BYTES//SLOT):
        raw=blob[index*SLOT:(index+1)*SLOT]
        magic,version,head,seq,us,previous,started,boot,size,crc,count,flags,_=HEADER.unpack_from(raw)
        if magic!=b'UKEFLT01':rejected['other_magic']+=1;continue
        if version!=1 or head!=128 or not seq or (seq-1)%256!=index or size>SLOT-head or not started or us<started or previous>us:
            rejected['header']+=1;continue
        checked=bytearray(raw[:head+size]);checked[92:96]=bytes(4)
        if zlib.crc32(checked)!=crc:rejected['crc']+=1;continue
        try:boot=str(uuid.UUID(boot.split(b'\0')[0].decode()))
        except (ValueError,UnicodeError):rejected['boot_uuid']+=1;continue
        pieces=[];at=head;valid=True
        while at<len(checked):
            if at+PIECE.size>len(checked):valid=False;break
            kind,n,cursor=PIECE.unpack_from(checked,at);at+=PIECE.size
            if not n or at+n>len(checked) or kind not in range(1,6):valid=False;break
            if (kind==1 and n%120) or (kind in (2,3) and n%72) or (kind in (4,5) and n!=256):valid=False;break
            pieces.append((kind,cursor,bytes(checked[at:at+n])));at+=n
        if not valid or at!=len(checked) or len(pieces)!=count:rejected['pieces']+=1;continue
        groups[(boot,started)].append(dict(sequence=seq,us=us,previous_us=previous,pieces=pieces,slot=index,flags=flags))
    for rows in groups.values():rows.sort(key=lambda x:x['sequence'])
    return groups,dict(rejected)


def extract(blob,out):
    groups,rejected=scan(blob)
    spec=importlib.util.spec_from_file_location('gpu_decoder',Path(__file__).resolve().parents[1]/'scripts/decode-gpu-trace.py')
    decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
    manifest=dict(bytes=len(blob),sha256=hashlib.sha256(blob).hexdigest(),rejected_slots=rejected,boots=[])
    for (boot,started),slots in groups.items():
        dest=out/f'{boot}-{started}';dest.mkdir(parents=True,exist_ok=True)
        gpu=bytearray();latency=bytearray();beats=[];cpus=[];windows=[];cursors={}
        for slot in slots:
            windows.append({k:v for k,v in slot.items() if k!='pieces'})
            for kind,cursor,data in slot['pieces']:
                cursors[kind]=cursor
                if kind==1:gpu.extend(data)
                elif kind in (2,3):latency.extend(data)
                elif kind==4:beats.append(dict(zip(BEAT_NAMES,struct.unpack('<32Q',data))))
                elif kind==5:cpus.append(dict(tick=cursor,us=slot['us'],cpus=[dict(state=a&0xffffffff,priority=a>>32,pending_signal=b,active_thread=hex(c),idle=bool(d)) for a,b,c,d in struct.iter_unpack('<4Q',data)]))
        gpu_rows=[]
        # Reuse the production decoder's bounded ABI without silently truncating
        # a continuous journal which may retain more than one RAM-ring length.
        for index,at in enumerate(range(0,len(gpu),65536*120)):
            part=gpu[at:at+65536*120];n=len(part)//120
            head=struct.pack('<4I2Q',0x31545047,1,120,n,cursors.get(1,0),beats[-1]['gpu_lost'] if beats else 0)
            (dest/f'gpu-{index}.trace.bin').write_bytes(head+part)
            _,rows=decoder.records(head+part);gpu_rows.extend(rows)
        (dest/'gpu-events.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in gpu_rows))
        head=struct.pack('<4I',0x3154414c,min(0xffffffff,cursors.get(2,0)+cursors.get(3,0)),len(latency)//72,
                         min(0xffffffff,beats[-1]['critical_lost']+beats[-1]['normal_lost']) if beats else 0)
        (dest/'kernel.latency.bin').write_bytes(head+latency)
        stats,rows=decoder.latency_records(head+latency)
        (dest/'latency.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
        (dest/'latency.analysis.json').write_text(json.dumps(decoder.latency_analysis(rows),indent=2)+'\n')
        (dest/'heartbeats.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in beats))
        (dest/'cpus.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in cpus))
        (dest/'slots.json').write_text(json.dumps(windows,indent=2)+'\n')
        summary=dict(boot_uuid=boot,started_us=started,first_sequence=slots[0]['sequence'],last_sequence=slots[-1]['sequence'],
            missing_slots=[seq for seq in range(slots[0]['sequence'],slots[-1]['sequence']+1) if seq not in {x['sequence'] for x in slots}],
            writer_first_us=slots[0]['us'],writer_last_us=slots[-1]['us'],gpu_records=len(gpu_rows),latency_records=len(rows),
            retained_event_first_us=min([r['us'] for r in rows]+[r['us'] for r in gpu_rows],default=None),
            retained_event_last_us=max([r['us'] for r in rows]+[r['us'] for r in gpu_rows],default=None),
            last_heartbeat=beats[-1] if beats else None,latency_stats=stats,directory=str(dest))
        manifest['boots'].append(summary)
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__);source=parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--serial');source.add_argument('--image',type=Path);parser.add_argument('--out',type=Path,required=True);args=parser.parse_args()
    if args.out.exists():parser.error('Output already exists; preserve previous captures')
    args.out.mkdir(parents=True)
    if args.image:blob=args.image.read_bytes()
    else:
        adb=['adb','-s',args.serial]
        def shell(*words):return subprocess.check_output(adb+['shell',*words],text=True,timeout=15).strip()
        if shell('getprop','ro.product.device')!='uke' or not shell('getprop','ro.twrp.version'):parser.error('Expected Uke in TWRP')
        sectors=int(shell('blockdev','--getsz','/dev/block/by-name/userdata'))
        if sectors!=471908312:parser.error('Unexpected userdata geometry')
        first=sectors//8-8192+4096;blob=bytearray()
        for at in range(0,4096,1024):
            data=subprocess.check_output(adb+['exec-out','dd','if=/dev/block/by-name/userdata','bs=4096',f'skip={first+at}','count=1024','status=none'],timeout=60)
            if len(data)!=1024*BLOCK:raise ValueError('Short ADB read')
            blob.extend(data)
        blob=bytes(blob)
    (args.out/'flight-region.bin').write_bytes(blob)
    report=extract(blob,args.out)
    print(json.dumps(report,indent=2))
    if not report['boots']:raise SystemExit('No valid flight journal slots')


if __name__=='__main__':main()
