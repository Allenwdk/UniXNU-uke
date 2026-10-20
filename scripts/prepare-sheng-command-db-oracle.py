#!/usr/bin/env python3
"""Generate ignored original-source command-db lookup/fixture oracle, no I/O."""
import hashlib,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def prepare():
 pins=json.loads((ROOT/'scripts/data/adreno-inputs.json').read_text());cache=ROOT/'.cache/adreno-source'
 names=['drivers/soc/qcom/cmd-db.c','include/soc/qcom/cmd-db.h']
 for n in names:
  if hashlib.sha256((cache/n).read_bytes()).hexdigest()!=pins['linux']['files'][n]['sha256']:raise SystemExit('Pinned command-db source mismatch')
 s=(cache/names[0]).read_text()
 def function(signature):
  start=s.index(signature);at=s.index('{',start)+1;depth=1
  while depth:
   if s[at]=='{':depth+=1
   elif s[at]=='}':depth-=1
   at+=1
  return s[start:at]
 types=''
 for name in ['entry_header','rsc_hdr','cmd_db_header']:
  start=s.index('struct '+name+' {');types+=s[start:s.index('\n};',start)+3]+'\n'
 prelude='''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "ShengGmuCommandDbSource.h"
typedef uint8_t u8;typedef uint16_t u16;typedef uint16_t __le16;typedef uint32_t __le32;typedef uint32_t u32;
#define NUM_PRIORITY 2
#define MAX_SLV_ID 8
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define __nonstring
#define EPROBE_DEFER 517
#define EINVAL 22
#define ENODEV 19
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define strtomem_pad(dst,src,pad) do {memset(dst,pad,sizeof(dst));size_t n=strlen(src);memcpy(dst,src,n<sizeof(dst)?n:sizeof(dst));}while(0)
'''+types+'''
_Static_assert(sizeof(struct entry_header)==24,"entry layout");
_Static_assert(sizeof(struct rsc_hdr)==16,"resource layout");
_Static_assert(sizeof(struct cmd_db_header)==144,"database layout");
static const u8 CMD_DB_MAGIC[]={0xdb,0x30,0x03,0x0c};
static struct cmd_db_header *cmd_db_header;
'''
 original='\n'.join(function(sig) for sig in ['static bool cmd_db_magic_matches(',
  'static inline const void *rsc_to_entry_header(', 'static inline void *\nrsc_offset(',
  'int cmd_db_ready(', 'static int cmd_db_get_header(', 'u32 cmd_db_read_addr(', 'const void *cmd_db_read_aux_data('])
 wrapper='''
void source_db_fixture(u8 *buffer,unsigned gmxc) {
 memset(buffer,0,SHENG_GMU_COMMAND_DB_BYTES);cmd_db_header=(void *)buffer;
 cmd_db_header->version=0x1234;memcpy(cmd_db_header->magic,CMD_DB_MAGIC,4);
 cmd_db_header->header[0]=(struct rsc_hdr){3,0,1024,(uint16_t)(gmxc?4:3),0x100,{0}};
 cmd_db_header->header[1]=(struct rsc_hdr){5,256,2048,4,0x100,{0}};
 const char *ids[8]={"gfx.lvl","mx.lvl","cx.lvl","gmxc.lvl","SH0","MC0","ACV","CN0"};
 for(unsigned group=0;group<2;++group) {
  struct rsc_hdr *r=cmd_db_header->header+group;
  struct entry_header *entries=(void *)(cmd_db_header->data+r->header_offset);
  for(unsigned j=0;j<r->cnt;++j) {
   struct entry_header *e=entries+j;const char *id=ids[group*4+j];
   memcpy(e->id,id,strlen(id));e->addr=(group?0x50000:0x30000)+4*j;
   e->offset=32*j;e->len=group?8:18;
   u8 *data=cmd_db_header->data+r->data_offset+e->offset;
   if(!group)for(unsigned k=0;k<9;++k){data[2*k]=(u8)(64*k);data[2*k+1]=(u8)((64*k)>>8);}
   else {data[0]=0xe8;data[1]=3;data[4]=j==0?16:4;data[6]=j?1:0;}
  }
 }
}
int source_db_lookup(u8 *buffer,const char *id,u32 *address,size_t *offset,size_t *len) {
 cmd_db_header=(void *)buffer;const void *aux=cmd_db_read_aux_data(id,len);
 if((intptr_t)aux<0)return -1;
 *address=cmd_db_read_addr(id);*offset=(const u8 *)aux-buffer;return 0;
}
void source_db_corrupt(u8 *buffer,unsigned control) {
 struct cmd_db_header *db=(void *)buffer;struct rsc_hdr *r=db->header;
 struct entry_header *arc=(void *)(db->data+r[0].header_offset),*bcm=(void *)(db->data+r[1].header_offset);
 switch(control) {
 case 0:db->magic[0]^=1;break;case 1:db->version++;break;
 case 2:r[0].cnt=65535;break;case 3:r[0].header_offset=65535;r[0].cnt=4096;break;
 case 4:r[1].header_offset=r[0].header_offset;break;case 5:r[0].slv_id=0;break;
 case 6:r[1].slv_id=3;break;case 7:r[0].data_offset=65535;arc[0].offset=65535;arc[0].len=65535;break;
 case 8:arc[0].offset=0;r[0].data_offset=0;break;case 9:arc[0].addr=0;break;
 case 10:arc[0].addr=0x30001;break;case 11:arc[0].addr=0x50000;break;
 case 12:memcpy(arc[1].id,arc[0].id,8);break;case 13:memset(bcm[0].id,'X',8);break;
 case 14:memcpy(bcm[0].id,arc[0].id,8);memset(arc[0].id,'X',8);break;
 case 15:arc[3].len=0;break; /* present but invalid, no fallback */
 case 16:bcm[1].addr=bcm[0].addr;break; /* semantic duplicate resource address */
 case 17:r[0].slv_id=6;break;
 }
}
'''
 out=ROOT/'build/sheng-gmu/command-db-oracle.c';out.parent.mkdir(exist_ok=True);out.write_text(prelude+original+wrapper)
 return out
if __name__=='__main__':prepare()
