// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "BootProgressCore.h"
#include "BootProgressDisplay.h"

int main(void) {
  struct bp_core s={.limit=120,.stall=30};
  bp_stage(&s,"start",0);
  assert(!bp_poll(&s,29,0,0,0));
  // Console spam does not postpone a stalled boot indefinitely.
  for(unsigned i=0;i<5000;++i) bp_char(&s,'X');
  assert(bp_poll(&s,30,0,0,0)==1);
  struct bp_core frozen=s;
  bp_stage(&s,"late",31);bp_char(&s,'Z');bp_poll(&s,100,9000,9000,0);
  assert(!memcmp(&s,&frozen,sizeof(s)));
  s=(struct bp_core){.limit=120,.stall=30};
  for(unsigned i=0;i<120;++i) assert(!bp_poll(&s,i,i,i*32,0));
  assert(bp_poll(&s,120,120,3840,0)==2); // active I/O is bounded too
  s=(struct bp_core){.limit=120,.stall=30,.continuous=1};
  bp_stage(&s,"continuous start",0);
  assert(bp_poll(&s,30,0,0,0)==1 && !s.frozen);
  bp_char(&s,'A');bp_stage(&s,"activity resumes",31);
  assert(!bp_poll(&s,31,1,1,0) && !s.frozen && s.logs[0][0]=='A');
  assert(bp_poll(&s,120,2,2,0)==2 && !s.frozen);
  bp_stage(&s,"after deadline",121);bp_char(&s,'B');
  assert(bp_poll(&s,600,3,3,0)==2 && !s.frozen && s.reads==3);
  assert(s.history_count==3 && s.logs[0][1]=='B');
  s=(struct bp_core){.limit=120,.stall=30};
  bp_stage(&s,"first",0);bp_stage(&s,"second",29);
  assert(!bp_poll(&s,58,0,0,0));assert(bp_poll(&s,59,0,0,0)==1);
  s=(struct bp_core){.limit=120,.stall=30};
  for(unsigned i=0;i<99;++i) { char b[20];snprintf(b,sizeof(b),"stage %u",i);bp_stage(&s,b,i); }
  assert(!strcmp(s.history[(s.history_count-1)%BP_HISTORY].text,"stage 98"));
  for(unsigned i=0;i<40;++i) { char b[20];snprintf(b,sizeof(b),"line %u\n",i);for(char *p=b;*p;++p)bp_char(&s,*p); }
  assert(!strcmp(s.logs[(s.log_row+BP_LOGS-1)%BP_LOGS],"line 39"));
  unsigned before=s.log_col;bp_char(&s,27);assert(s.log_col==before);
  // Actual production glyph renderer, guard bytes and padded scanlines.
  for(unsigned scale=1;scale<=3;scale+=2) {
    unsigned w=720*scale,h=700*scale,rb=w*4+64;
    size_t size=(size_t)h*rb;
    unsigned char *mem=malloc(size+256);assert(mem);memset(mem,0x5a,size+256);
    struct bp_display d={.frame=(uintptr_t)(mem+128),.bound=1,.width=w,.height=h,.stride=rb,.scale=scale};
    bp_clear(&d);
    for(unsigned i=0;i<48;++i) { memset(d.previous[i],0xff,BP_COLS-1);d.previous[i][BP_COLS-1]=0; }
    bp_line(&d,0,"TIME 120 UFS 0123456789 ABC xyz",0xffffffff);
    bp_line(&d,47,"last line",0xffffffff);bp_line(&d,48,"out of range",0xffffffff);
    for(unsigned i=0;i<128;++i) assert(mem[i]==0x5a && mem[128+size+i]==0x5a);
    for(unsigned y=0;y<h;++y) for(unsigned x=w*4;x<rb;++x) assert(mem[128+(size_t)y*rb+x]==0x5a);
    unsigned white=0;for(unsigned y=0;y<12*scale;++y)for(unsigned x=0;x<5*scale;++x)
      white+=*(uint32_t*)(mem+128+(size_t)y*rb+x*4)==0xffffffff;
    assert(white>0);
    unsigned char *copy=malloc(size);assert(copy);memcpy(copy,mem+128,size);
    bp_line(&d,0,"TIME 120 UFS 0123456789 ABC xyz",0xffffffff);
    assert(!memcmp(copy,mem+128,size)); // identical line avoids device traffic
    d.yield_requested=1;
    bp_line(&d,0,"must yield",0xffffffff);bp_clear(&d);
    assert(!memcmp(copy,mem+128,size));
    assert(!strcmp(d.previous[0],"TIME 120 UFS 0123456789 ABC xyz"));
    d.yield_requested=0;
    bp_line(&d,0,"",0xffffffff);
    for(unsigned y=0;y<14*scale;++y)for(unsigned x=0;x<(BP_COLS-1)*6*scale;++x)
      assert(*(uint32_t*)(mem+128+(size_t)y*rb+x*4)==0);
    free(copy);free(mem);
  }
  puts("PASS: stalled/active deadlines, immutable stop, log/history rollover, actual glyph stores and padded framebuffer bounds");
}
