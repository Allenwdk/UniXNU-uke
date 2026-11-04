#!/usr/bin/env python3
"""Execute the actual touch stream emitter, including its preprocessor guards."""
from pathlib import Path
import argparse
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path,default=root/'package/BootDiagExport.c')
args=parser.parse_args()
s=args.source.read_text()
start=s.index('  if (!rc && touch_keep) {',s.index('int boot_diag_stream('))
guard=s.rfind('#if',0,start)
if guard>=0 and '#endif' not in s[guard:start]:start=guard
end=s.index('  if (!rc && client_keep) {',start)
body=s[start:end]
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
enum {TouchRing=65536};
static uint32_t touch_kept,touch_keep;
static uint64_t ring[TouchRing];
static volatile uint64_t *native_touch_report_ring=ring;
static unsigned char output[600000],chunk[65536];static size_t used;
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns){*ns=t*1000;}
static int emit(const void *p,uint32_t n,void *arg){(void)arg;assert(used+n<=sizeof(output));memcpy(output+used,p,n);used+=n;return 0;}
static int put_head(int (*fn)(const void *,uint32_t,void *),void *arg,const char *path,uint32_t mode,uint32_t uid,uint32_t gid,uint64_t size){
 uint32_t h[4]={(uint32_t)strlen(path),mode,uid,gid};unsigned char header[24];memcpy(header,h,16);memcpy(header+16,&size,8);
 return fn(header,24,arg)||fn(path,(uint32_t)strlen(path),arg);
}
static int stream(void){int rc=0;void *arg=0;
'''
main=r'''
 return rc;}
static void check(uint32_t total){
 touch_kept=total;touch_keep=total!=0;used=0;
 for(uint32_t i=0;i<total;++i)ring[i&(TouchRing-1)]=17+(uint64_t)i*100;
 assert(!stream());
 if(!total){assert(used==0);return;}
 const uint32_t kept=total>TouchRing?TouchRing:total;
 assert(used==24+strlen("kernel.touch.bin")+16+(size_t)kept*8);
 uint32_t head[4];memcpy(head,output+40,16);
 assert(head[0]==0x31484354 && head[1]==total && head[2]==kept);
 for(uint32_t i=0;i<kept;++i){uint64_t t;memcpy(&t,output+56+(size_t)i*8,8);assert(t==17+(uint64_t)(total-kept+i)*100);}
}
int main(void){check(0);check(630);check(65539);puts("PASS: actual touch export emitter, 630-record length5096, chronological ring wrap and disabled-empty case");}
'''
with tempfile.TemporaryDirectory(prefix='uke-touch-export-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(prefix+body+main)
    subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
