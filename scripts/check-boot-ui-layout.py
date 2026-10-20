#!/usr/bin/env python3
"""Execute the actual diagnostic middle layout and reject old-log underpainting."""
import hashlib,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1];out=root/'build/cpu-desktop/ui-layout';out.mkdir(parents=True,exist_ok=True)
s=(root/'kits/uke/BootProgress.c').read_text();body=s[s.index('  if(!__atomic_exchange_n(&exit_lock'):s.index('  if(&boot_process_rows)')]
prefix=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "BootProgressCore.h"
#include "BootProgressLifecycle.h"
static struct bp_exits exit_records,exit_snapshot;
static unsigned exit_lock,exit_dropped;
static unsigned boot_ui_trace_ready;
static volatile uint32_t native_legacy_frame_count,native_legacy_frame_colors[8][4];
static volatile uint64_t native_legacy_frame_color_counts[8][4],native_legacy_frame_color_other[8];
static char boot_ui_trace_rows[14][120],boot_ui_trace_label[120];
struct pc_sample {unsigned second;int pid;char name[16];uint64_t pc,psr;};
static struct pc_sample samples[8];static unsigned sample_count;
static unsigned writes[48];static char final[48][120];
static void line(unsigned row,const char *text,uint32_t color){(void)color;assert(row<48);++writes[row];snprintf(final[row],120,"%s",text);}
static void layout(struct bp_core *visible){char b[120];
'''
main=r'''
}
int main(void){
 struct bp_core core={0};
 for(unsigned i=0;i<14;++i)snprintf(boot_ui_trace_rows[i],120,"TRACE %u",i);
 snprintf(boot_ui_trace_label,120,"STACK64 1/2");
 for(unsigned pass=0;pass<4;++pass){
  boot_ui_trace_ready=1;memset(writes,0,sizeof(writes));
  if(pass==1){exit_records.total=1;exit_records.failed=1;strcpy(exit_records.failures[0],"p86 SIGSEGV");}
  if(pass==3){exit_records.crashes=1;strcpy(exit_records.crash[0],"p86 SIGSEGV PC100012340");strcpy(exit_records.failures[0],"ordinary SIGKILL");}
  if(pass==2)exit_lock=1;else exit_lock=0;
  layout(&core);
  for(unsigned row=6;row<=23;++row)assert(writes[row]==1);
  for(unsigned row=6;row<20;++row)assert(!strcmp(final[row],boot_ui_trace_rows[row-6]));
  assert(!strcmp(final[20],boot_ui_trace_label));
  if(pass==3)assert(strstr(final[21],"SIGSEGV PC100012340"));
 }
 boot_ui_trace_ready=0;exit_records=(struct bp_exits){0};exit_lock=0;memset(writes,0,sizeof(writes));layout(&core);
 assert(strstr(final[16],"Recent kernel console")&&writes[6]&&writes[23]);
 puts("PASS: actual middle layout renders each stack/exit row once, with no old debug underpainting; empty, exit, contended and old-format states");
}
'''
(out/'test.c').write_text(prefix+body+main)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-pointer-bool-conversion','-Wno-unused-function','-fsanitize=address,undefined','-I',str(root/'kits/uke'),str(out/'test.c'),'-o',str(out/'test')],check=True,timeout=45)
r=subprocess.run([str(out/'test')],text=True,capture_output=True,check=True,timeout=15);print(r.stdout,end='')
(out/'report.json').write_text(json.dumps(dict(passed=True,layout_body_sha256=hashlib.sha256(body.encode()).hexdigest(),pixel_writes_substituted=True,hardware_flicker_fix_observed=False),indent=2)+'\n')
