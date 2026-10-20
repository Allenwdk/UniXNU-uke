#!/usr/bin/env python3
"""Execute actual process-stage and exit recorder helpers with clock/render stubs."""
from pathlib import Path
import hashlib,json,subprocess
root=Path(__file__).resolve().parents[1];out=root/'build/cpu-desktop/lifecycle';out.mkdir(parents=True,exist_ok=True)
s=(root/'kits/uke/BootProgress.c').read_text();body=s[s.index('void boot_progress_process_stage('):s.index('void boot_progress_irq(')]
prefix=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "BootProgressCore.h"
#include "BootProgressLifecycle.h"
static struct bp_core state;
static struct bp_exits exit_records;
static unsigned enabled=2,started=1,exit_lock,exit_dropped;
static void (*boot_progress_exit_fault)(int,unsigned,char *,unsigned long);
static int recorder_busy;
static int take(void){return !recorder_busy;}
static void release(void){}
static unsigned seconds(void){return 701;}
static void draw(unsigned now){(void)now;}
'''
main=r'''
int main(void){
 boot_progress_process_stage(351,"xpcproxy","exec process signature accepted");
 assert(state.history_count==1&&strstr(state.history[0].text,"target pid=351 xpcproxy")&&state.history[0].seconds==701);
 recorder_busy=1;boot_progress_process_stage(1,"launchd","busy");assert(state.history_count==1);recorder_busy=0;
 boot_progress_exit(351,"helper",0,0,0);assert(exit_records.total==1&&!exit_records.failed);
 boot_progress_exit(352,"watchdogd",5,2,5);assert(exit_records.failed==1&&strstr(exit_records.failures[0],"sig=5 exit=0 ns=2 sub=5"));
 for(int i=0;i<1000;++i)boot_progress_exit(400+i,"successful helper",0,0,0);
 assert(exit_records.total==1002&&exit_records.failed==1&&strstr(exit_records.failures[0],"watchdogd"));
 boot_progress_exit(1400,"daemon",256,0,0);assert(strstr(exit_records.failures[1],"sig=0 exit=1"));
 for(int i=0;i<1000;++i)boot_progress_exit(2000+i,"ordinary SIGKILL",9,2,9);
 assert(exit_records.crashes==1&&strstr(exit_records.crash[0],"watchdogd"));
 boot_progress_exit(3001,"IOMFB_bics_daemon",11,2,11);
 assert(exit_records.crashes==2&&strstr(exit_records.crash[1],"sig=11"));
 struct bp_exits before=exit_records;exit_lock=1;boot_progress_exit(1,"launchd",9,2,9);assert(!memcmp(&before,&exit_records,sizeof(before))&&exit_dropped==1);exit_lock=0;
 enabled=0;boot_progress_exit(1,"ignored",9,2,9);assert(!memcmp(&before,&exit_records,sizeof(before)));
 puts("PASS: actual target-process attribution, gate refusal, exit signal/status/reason formatting, failure retention across 1000 successful helpers plus separate crash-signal retention across 1000 SIGKILL exits and contention accounting");
}
'''
(out/'test.c').write_text(prefix+body+main)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-O1','-g','-fsanitize=address,undefined','-I',str(root/'kits/uke'),str(out/'test.c'),'-o',str(out/'test')],check=True,timeout=45)
r=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,timeout=10);print(r.stdout,end='')
(out/'report.json').write_text(json.dumps(dict(passed=True,helper_sha256=hashlib.sha256(body.encode()).hexdigest(),clock_render_dependencies_stubbed=True,hardware_exit_causes_observed=False),indent=2)+'\n')
