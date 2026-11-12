/* SPDX-License-Identifier: MIT */
#ifndef UKE_FLIGHT_STORE_H
#define UKE_FLIGHT_STORE_H
#include <stdint.h>
#include <string.h>
enum {FlBlock=4096,FlSlotBlocks=16,FlSlotBytes=65536,FlHeaderBytes=128,
      FlFirstBlock=4096,FlRegionBlocks=4096,FlSlots=256};
struct fl_header {
 char magic[8];uint32_t version,header_bytes;
 uint64_t sequence,us,previous_us,started_us;
 char boot[40];uint32_t bytes,crc,records,flags;uint8_t reserved[24];
};
struct fl_piece {uint32_t kind,bytes;uint64_t cursor;};
/* kind1 GPU records (120B); kind2/3 critical/normal latency records (72B);
 * kind4 writer heartbeat (u64 words); kind5 CPU/worker status (u64 words). */
typedef int (*fl_write_fn)(uint64_t,uint32_t,const void *,void *);
struct fl_writer {uint8_t *buffer;uint32_t used,records;uint64_t sequence,last_us,started_us;char boot[40];fl_write_fn write;void *context;};
static inline uint32_t fl_crc32(const void *data,uint32_t size,uint32_t crc) {
 static const uint32_t table[16]={0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
 const uint8_t *p=(const uint8_t *)data;
 while(size--){crc^=*p++;crc=(crc>>4)^table[crc&15];crc=(crc>>4)^table[crc&15];}
 return crc;
}
static inline int fl_flush(struct fl_writer *w,uint64_t us) {
 if(w->used==FlHeaderBytes)return 1;
 struct fl_header *h=(struct fl_header *)w->buffer;memset(h,0,sizeof(*h));
 memcpy(h->magic,"UKEFLT01",8);h->version=1;h->header_bytes=FlHeaderBytes;
 h->sequence=w->sequence+1;h->us=us;h->previous_us=w->last_us;h->started_us=w->started_us;
 memcpy(h->boot,w->boot,sizeof h->boot);h->bytes=w->used-FlHeaderBytes;h->records=w->records;
 h->crc=~fl_crc32(w->buffer,w->used,~0u);
 const uint32_t blocks=(w->used+FlBlock-1)/FlBlock;
 memset(w->buffer+w->used,0,blocks*FlBlock-w->used);
 if(!w->write(FlFirstBlock+(w->sequence%FlSlots)*FlSlotBlocks,blocks,w->buffer,w->context))return 0;
 ++w->sequence;w->last_us=us;w->used=FlHeaderBytes;w->records=0;return 1;
}
static inline int fl_emit(struct fl_writer *w,uint32_t kind,uint64_t cursor,const void *data,uint32_t size,uint64_t us) {
 if(!size || size>FlSlotBytes-FlHeaderBytes-sizeof(struct fl_piece))return 0;
 if(w->used+sizeof(struct fl_piece)+size>FlSlotBytes && !fl_flush(w,us))return 0;
 struct fl_piece piece={kind,size,cursor};memcpy(w->buffer+w->used,&piece,sizeof piece);w->used+=sizeof piece;
 memcpy(w->buffer+w->used,data,size);w->used+=size;++w->records;return 1;
}
#endif
