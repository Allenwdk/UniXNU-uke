#!/usr/bin/env python3
"""Execute the production pulse loop, pixel writer and handoff on ARM64 host.

Clock, scheduler and recorder dependencies are substituted. The DSB barrier
in the actual handoff is legal at EL0; no privileged register access executes.
"""
from pathlib import Path
import hashlib
import json
import platform
import subprocess

root=Path(__file__).resolve().parents[1]
assert platform.system()=='Darwin' and platform.machine()=='arm64'
out=root/'build/cpu-desktop/pulse-host';out.mkdir(parents=True,exist_ok=True)
source=(root/'kits/uke/BootProgress.c').read_text()
handoff=source[source.index('int boot_progress_active('):source.index('\nvoid boot_progress_cancel_scanout(')]
worker=source[source.index('static void pulse_monitor('):source.index('\nstatic void process_monitor(')]
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include "BootProgressCore.h"
#include "BootProgressDisplay.h"
#include "BootProgressPulse.h"
static struct bp_display display;
static struct bp_pulse pulse;
static struct bp_panic_info boot_progress_panic_vinfo;
static unsigned enabled=2,busy,render_busy,boot_progress_display_owned,last_scanout_at;
static unsigned pulse_worker_status,bind_count,bind_result=3,bind_depth;
static uintptr_t bind_base;
static uint64_t irq_entries=116000,irq_entry_pc=0xfffffff00bf70000ULL,thread_count=65;
static unsigned now,rounds;
static jmp_buf done;
typedef int wait_result_t;
typedef void *event_t;
#define THREAD_UNINT 0
#define NSEC_PER_MSEC 1000000
#define THREAD_CONTINUE_NULL 0
static int take(void){return !__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE);}
static void release(void){__atomic_store_n(&busy,0,__ATOMIC_RELEASE);}
static unsigned seconds(void){return now;}
static void *current_thread(void){return NULL;}
static void thread_set_thread_name(void *t,const char *n){(void)t;assert(!strcmp(n,"boot_scanout_pulse"));}
static void assert_wait_timeout(event_t e,int mode,int count,int unit){
 assert(e==&pulse_worker_status && mode==THREAD_UNINT && count==1000 && unit==NSEC_PER_MSEC);
}
static void thread_block(int c){assert(c==THREAD_CONTINUE_NULL);++now;irq_entries+=100;if(++rounds==4)longjmp(done,1);}
static void line(unsigned row,const char *text,uint32_t color){bp_line(&display,row,text,color);}
'''
main=r'''
int main(void){
 const unsigned w=3048,h=2032,rb=w*4+64;
 const size_t bytes=(size_t)h*rb;
 unsigned char *memory=malloc(bytes+256),*copy=malloc(bytes+256);assert(memory&&copy);
 memset(memory,0x5a,bytes+256);memcpy(copy,memory,bytes+256);
 uintptr_t base=(uintptr_t)(memory+128);
 assert(!boot_progress_pulse_anchor(base,bytes-1,w,h,rb));
 assert(!boot_progress_pulse_anchor(base+1,bytes,w,h,rb));
 assert(!boot_progress_pulse_anchor(UINTPTR_MAX-3,bytes,w,h,rb));
 assert(boot_progress_pulse_anchor(base,bytes,w,h,rb));
 assert(boot_progress_panic_vinfo.frame==base && boot_progress_panic_vinfo.height==h &&
        boot_progress_panic_vinfo.width==w && boot_progress_panic_vinfo.stride==rb && boot_progress_panic_vinfo.depth==32);
 assert(!boot_progress_pulse_anchor(base,bytes,w,h,rb));
 assert(!memcmp(copy,memory,bytes+256)); // Registration never clears the frame.
 display=(struct bp_display){.frame=base,.bound=1,.width=w,.height=h,.stride=rb,.scale=3};
 // Freeze the recorder, normal renderer and normal monitor. The actual pulse
 // still runs using its retained mapping and synthetic advancing IRQ count.
 busy=render_busy=1;
 if(!setjmp(done))pulse_monitor(NULL,0);
 assert(pulse_worker_status==2 && strstr(pulse.screen.previous[0],"P4 I116300 M65/3s"));
 assert(strstr(pulse.screen.previous[0],"B0:3") && strstr(pulse.screen.previous[0],"lock1/1"));
 assert(!memcmp(memory,copy,128+(size_t)(h-32)*rb));
 for(unsigned i=0;i<128;++i)assert(memory[128+bytes+i]==0x5a);
 for(unsigned y=h-32;y<h;++y)for(unsigned x=w*4;x<rb;++x)assert(memory[128+(size_t)y*rb+x]==0x5a);
 assert(!boot_progress_prepare_scanout() && !pulse.closed && !boot_progress_display_owned);
 render_busy=0;assert(bp_pulse_enter(&pulse));
 assert(!boot_progress_prepare_scanout() && !render_busy && !pulse.closed);
 bp_pulse_leave(&pulse);
 assert(boot_progress_prepare_scanout() && pulse.closed && !pulse.ready && !display.bound);
 assert(boot_progress_display_owned && !render_busy && !pulse.busy);
 memcpy(copy,memory,bytes+256);
 bp_line(&pulse.screen,0,"must not overwrite the UI",0xffffffff);
 assert(!boot_progress_pulse_anchor(base,bytes,w,h,rb) && !memcmp(copy,memory,bytes+256));
 enabled=0;assert(boot_progress_prepare_scanout());
 free(copy);free(memory);
 puts("PASS: actual independent pulse loop survives recorder/render stalls; bounded real glyph stores, padded rows, contention refusal and permanent handoff closure");
}
'''
c=out/'test.c';exe=out/'test';c.write_text(prefix+handoff+worker+main)
subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
 '-fsanitize=address,undefined','-I'+str(root/'kits/uke'),
 '-I'+str(root/'uefi-sheng/Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib'),
 str(c),'-o',str(exe)],check=True,timeout=30)
r=subprocess.run([str(exe)],capture_output=True,text=True,check=True,timeout=15);print(r.stdout,end='')
(out/'report.json').write_text(json.dumps({'passed':True,'handoff_sha256':hashlib.sha256(handoff.encode()).hexdigest(),
 'pulse_worker_sha256':hashlib.sha256(worker.encode()).hexdigest(),'dependencies_substituted':True,
 'real_glyph_stores':True,'hardware_observed':False,'device_operation':False},indent=2)+'\n')
