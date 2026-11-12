#!/usr/bin/env python3
"""One read-only recovery collection: flight first, full exports, raw text ring."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--serial',required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--baseline-log',type=Path);a=p.parse_args()
    if a.out.exists():p.error('Output already exists')
    adb=['adb','-s',a.serial]
    def shell(*words):return subprocess.check_output(adb+['shell',*words],text=True,timeout=15).strip()
    if shell('getprop','ro.product.device')!='uke' or not shell('getprop','ro.twrp.version'):p.error('Expected Uke in TWRP')
    sectors=int(shell('blockdev','--getsz','/dev/block/by-name/userdata'))
    if sectors!=471908312:p.error('Unexpected userdata geometry')
    a.out.mkdir(parents=True);tools=Path(__file__).resolve().parent;status={}
    for name,script in [('flight','read-flight.py'),('full-exports','read-frame-path.py')]:
        with (a.out/f'{name}-read.log').open('w') as log:
            r=subprocess.run([sys.executable,tools/script,'--serial',a.serial,'--out',a.out/name],stdout=log,stderr=subprocess.STDOUT)
        status[name]=dict(exit_code=r.returncode,log=str(a.out/f'{name}-read.log'))
    raw=bytearray();base=sectors//8-8192
    for at in range(0,8192,1024):
        data=subprocess.check_output(adb+['exec-out','dd','if=/dev/block/by-name/userdata','bs=4096',f'skip={base+at}','count=1024','status=none'],timeout=60)
        if len(data)!=1024*4096:raise ValueError('Short raw log read')
        raw.extend(data)
    (a.out/'ukelog.bin').write_bytes(raw)
    baseline=a.baseline_log.read_bytes() if a.baseline_log else None
    if baseline is not None and len(baseline)!=len(raw):raise ValueError('Baseline log geometry differs')
    rows=[]
    # This candidate reserves the upper 16 MiB for flight slots. All raw bytes
    # remain archived, but only the lower half can contain its text records.
    for at in range(0,16*1024*1024,16384):
        block=raw[at:at+16384]
        if baseline is not None and block==baseline[at:at+16384]:continue
        if block[:8]!=b'UKELLOG1':continue
        seq,size,crc=struct.unpack_from('<QII',block,8)
        if not 0<size<=16352:continue
        body=block[32:32+size]
        if zlib.crc32(body)!=crc:continue
        rows.append(dict(sequence=seq,offset=at,text=body.decode(errors='replace')))
    rows.sort(key=lambda x:x['sequence'])
    (a.out/'text-records.json').write_text(json.dumps(rows,indent=2)+'\n')
    (a.out/'kernel-text.log').write_text(''.join(r['text'] for r in rows))
    status.update(raw_bytes=len(raw),raw_sha256=hashlib.sha256(raw).hexdigest(),text_records=len(rows),
                  baseline=str(a.baseline_log) if a.baseline_log else None,
                  text_current_boot_proven=False,read_only=True,host_userdata_written=False)
    (a.out/'collection.json').write_text(json.dumps(status,indent=2)+'\n');print(json.dumps(status,indent=2))
    if status['flight']['exit_code']:raise SystemExit('Flight collection needs inspection; other evidence was preserved')


if __name__=='__main__':main()
