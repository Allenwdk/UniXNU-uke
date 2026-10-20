#!/usr/bin/env python3
"""Run actual fault capture/exit formatting against explicit kernel API stand-ins."""
import hashlib,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1];out=root/'build/cpu-desktop/fault-probe';out.mkdir(parents=True,exist_ok=True)
s=(root/'kits/uke/BootFaultProbe.c').read_text();body=s[s.index('struct boot_fault_record'):s.index('// REASON-SCAN')]
prefix=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
struct user64_dyld_all_image_infos {unsigned version,infoArrayCount;uint64_t infoArray;};
struct user64_dyld_image_info {uint64_t imageLoadAddress,imageFilePath,imageFileModDate;};
static struct user64_dyld_all_image_infos infos;
#define task_get_all_image_info_addr(t) ((uint64_t)(uintptr_t)&infos)
struct saved { uint64_t pc,lr,sp,fp,x[29]; };typedef struct saved arm_saved_state_t;
#define get_saved_state_pc(s) ((s)->pc)
#define get_saved_state_lr(s) ((s)->lr)
#define get_saved_state_sp(s) ((s)->sp)
#define get_saved_state_fp(s) ((s)->fp)
#define get_saved_state_reg(s,i) ((s)->x[i])
static struct {uint64_t task_shared_region_slide;} task;
#define current_task() (&task)
#define current_thread() ((void *)0)
#define find_user_regs(t) ((arm_saved_state_t *)0)
#define is_saved_state64(s) 1
static int pid=158,copyfail,copycalls;
static const char *name="watchdogd";
static int proc_selfpid(void){return pid;}
static void proc_selfname(char *p,int n){snprintf(p,(unsigned)n,"%s",name);}
static int copyin(uint64_t p,void *out,unsigned long n){++copycalls;if(copyfail||p<4096)return 14;memcpy(out,(void *)(uintptr_t)p,n);return 0;}
static int copyinstr(uint64_t p,char *out,unsigned long n,unsigned long *actual){++copycalls;if(copyfail||p<4096)return 14;unsigned long z=strlen((char *)(uintptr_t)p)+1;if(z>n)return 63;memcpy(out,(void *)(uintptr_t)p,z);*actual=z;return 0;}
'''
main=r'''
int main(void){
 char out[1024]="watchdogd[158] exited";
 boot_progress_critical_exit(158,out,sizeof(out));assert(strstr(out,"unavailable")&&!copycalls);
 uint32_t instruction=0xd4200020;uint64_t stack[2]={0,0x100006830};
 arm_saved_state_t state={.pc=(uintptr_t)&instruction,.lr=0x10000ab0c,.fp=(uintptr_t)stack,.sp=0x170000000};
 struct user64_dyld_image_info im={0x100020000,(uintptr_t)"/usr/libexec/watchdogd",0};infos.infoArrayCount=1;infos.infoArray=(uintptr_t)&im;
 state.x[0]=(uintptr_t)"missing service\nassert";state.x[20]=0xe00002c7;task.task_shared_region_slide=0x1234000;
 boot_progress_user_breakpoint(&state,0xf2000001);assert(copycalls==0&&fault_count==1);
 state.pc=0;state.lr=0; // The record must survive saved-state mutation.
 strcpy(out,"watchdogd[158] exited");boot_progress_critical_exit(158,out,sizeof(out));
 assert(strstr(out,"EL0 watchdogd")&&strstr(out,"LR=10000ab0c")&&strstr(out,"ESR=f2000001")&&strstr(out,"cache+=1234000"));
 assert(strstr(out,"image0=100020000 err=0 /usr/libexec/watchdogd"));
 assert(strstr(out,"INSN=d4200020 err=0")&&strstr(out,"x20=e00002c7")&&strstr(out,"frames=1 0/100006830")&&strstr(out,"missing service?assert"));
 struct {char b[24];char guard[16];} bounded;memset(&bounded,'Z',sizeof(bounded));strcpy(bounded.b,"exited");boot_progress_critical_exit(158,bounded.b,sizeof(bounded.b));assert(bounded.b[23]==0);for(unsigned i=0;i<16;++i)assert(bounded.guard[i]=='Z');
 copyfail=1;strcpy(out,"exited");boot_progress_critical_exit(158,out,sizeof(out));assert(strstr(out,"err=14")&&strstr(out,"frames=0")&&strstr(out,"x0str err=14"));copyfail=0;
 int before=copycalls;pid=200;strcpy(out,"exited");boot_progress_critical_exit(158,out,sizeof(out));assert(copycalls==before&&strstr(out,"err=-1"));
 fault_lock=1;before=fault_count;boot_progress_user_breakpoint(&state,1);assert(fault_count==(unsigned)before);strcpy(out,"exited");boot_progress_critical_exit(158,out,sizeof(out));assert(!strcmp(out,"exited"));fault_lock=0;
 for(int i=0;i<12;++i){pid=300+i;boot_progress_user_breakpoint(&state,1);}strcpy(out,"exited");boot_progress_critical_exit(158,out,sizeof(out));assert(strstr(out,"unavailable"));
 pid=86;name="IOMFB_bics_daemon";state.pc=0x100012340;state.lr=0x100045670;
 before=copycalls;boot_progress_user_abort(&state,0x92000006,0xdeadbeef);
 assert(copycalls==before);state.pc=0;state.lr=0;
 strcpy(out,"prior");boot_progress_exit_fault(86,11,out,120);
 assert(strstr(out,"sig11 PC100012340 LR100045670 E92000006 Fdeadbeef +1234000"));
 before=copycalls;strcpy(out,"prior");boot_progress_exit_fault(87,11,out,120);assert(!strcmp(out,"prior")&&copycalls==before);
 fault_lock=1;boot_progress_exit_fault(86,11,out,120);assert(!strcmp(out,"prior"));fault_lock=0;
 memset(&bounded,'Z',sizeof(bounded));boot_progress_exit_fault(86,11,bounded.b,sizeof(bounded.b));assert(bounded.b[23]==0);for(unsigned i=0;i<16;++i)assert(bounded.guard[i]=='Z');
 puts("PASS: actual breakpoint/unresolved-abort capture and critical/noncritical exit formatters; immutable snapshot, PID matching, no trap-time user reads, try-once gates, copy failures, ring overwrite and guarded output bounds");
}
'''
(out/'test.c').write_text(prefix+body+main)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],check=True,timeout=60)
r=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,timeout=15)
(out/'report.json').write_text(json.dumps(dict(source_body_sha256=hashlib.sha256(body.encode()).hexdigest(),passed=True,kernel_APIs_substituted=True,hardware_observed=False),indent=2)+'\n');print(r.stdout,end='')
