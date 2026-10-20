#!/usr/bin/env python3
"""Execute the actual refresh() resume gate with display/recorder stand-ins."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1];out=root/'build/cpu-desktop/diag-resume';out.mkdir(parents=True,exist_ok=True)
s=(root/'kits/uke/BootProgress.c').read_text()
body=s[s.index('static void refresh('):s.index('\nstatic void pulse_monitor(')] if '\nstatic void pulse_monitor(' in s else None
body=s[s.index('static void refresh('):s.index('\n}\n',s.index('static void refresh('))+3]
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define BP_COLS 120
#define ACQ __ATOMIC_ACQUIRE
struct bp_display {uintptr_t frame;unsigned bound;char previous[48][BP_COLS];};
static struct bp_display display;
static unsigned render_busy,resume_after,last_scanout_at,clears,draws,taken;
static volatile unsigned boot_progress_display_owned;
static int take(void){++taken;return 1;}
static void release(void){}
static void bp_clear(struct bp_display *d){assert(d->bound);++clears;}
static void draw(unsigned now){(void)now;++draws;}
struct snap {int x;};static struct snap state,render_snapshot;
'''
main=r'''
int main(void){
 display.frame=0x1000;boot_progress_display_owned=1;last_scanout_at=100;
 resume_after=0;refresh(500);assert(!display.bound&&!clears&&draws==1&&!render_busy);
 resume_after=10;refresh(105);assert(!display.bound&&!clears);
 refresh(109);assert(!display.bound&&!clears);
 refresh(110);assert(display.bound&&clears==1&&display.previous[0][0]==(char)0xff&&display.previous[47][BP_COLS-1]==0);
 refresh(111);assert(clears==1);
 display.bound=0;boot_progress_display_owned=0;refresh(900);assert(!display.bound&&clears==1);
 boot_progress_display_owned=1;display.frame=0;refresh(900);assert(!display.bound&&clears==1);
 display.frame=0x1000;render_busy=1;unsigned d=draws;refresh(900);assert(!display.bound&&draws==d&&render_busy==1);
 return 0;
}
'''
(out/'test.c').write_text(prefix+body+main)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-O1','-g','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],check=True,timeout=45)
subprocess.run([str(out/'test')],check=True,timeout=15)
print('PASS: actual refresh resume gate; disabled, idle threshold, one clear/rebind, owned/frame/busy refusals; display and recorder stand-ins')
