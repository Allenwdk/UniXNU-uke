#!/usr/bin/env python3
"""Exercise the actual read-only probe body with bounded kernel API stand-ins."""
from pathlib import Path
import subprocess,hashlib,json
root=Path(__file__).resolve().parents[1];out=root/'build/cpu-desktop/process-probe';out.mkdir(parents=True,exist_ok=True)
s=(root/'kits/uke/BootProcessProbe.c').read_text();body=s[s.index('char boot_process_rows'):]
prefix=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define PROC_RETURNED 0
#define PROC_ALLPROCLIST 1
#define PROC_NOWAITTRANS 4
#define TH_RUN 4
#define TH_WAIT 1
struct state {uint64_t pc;};
struct thread {unsigned state;int sched_pri;uint64_t wait_event;void(*continuation)(void);struct {struct state *upcb;} machine;bool sched_lock;const char *name;struct thread *next;};
typedef struct thread *thread_t;
struct task {struct thread *threads;unsigned thread_count,task_shared_region_slide;bool busy;};typedef struct task *task_t;
struct proc {const char *name;int pid;task_t task;};typedef struct proc *proc_t;
static struct proc procs[4];static unsigned procCount;static bool interrupts=true;
static int thread_lck_grp;
static unsigned host_focus;
static char host_focus_name[32],host_focus2_name[32],host_focus3_name[32],host_focus4_name[32];
static int __attribute__((unused)) PE_parse_boot_argn(const char *name,void *p,int size){
 if(!strcmp(name,"boot-ui-focus")){assert(size==32);memcpy(p,host_focus_name,32);return host_focus_name[0]!=0;}
 if(!strcmp(name,"boot-ui-focus2")){assert(size==32);memcpy(p,host_focus2_name,32);return host_focus2_name[0]!=0;}
 if(!strcmp(name,"boot-ui-focus3")){assert(size==32);memcpy(p,host_focus3_name,32);return host_focus3_name[0]!=0;}
 if(!strcmp(name,"boot-ui-focus4")){assert(size==32);memcpy(p,host_focus4_name,32);return host_focus4_name[0]!=0;}
 assert(!strcmp(name,"boot-ui-chrono")&&size==4);memcpy(p,&host_focus,4);return host_focus!=0;}
static const char *proc_best_name(proc_t p){return p->name;}
static int proc_pid(proc_t p){return p->pid;}
static task_t proc_task(proc_t p){return p->task;}
static bool task_lock_try(task_t t){if(t->busy)return false;t->busy=true;return true;}
static void task_unlock(task_t t){assert(t->busy);t->busy=false;}
static bool ml_set_interrupts_enabled(bool value){bool old=interrupts;interrupts=value;return old;}
static bool simple_lock_try(bool *busy,int *grp){assert(!interrupts&&grp==&thread_lck_grp);if(*busy)return false;*busy=true;return true;}
static void thread_unlock(thread_t t){assert(!interrupts&&t->sched_lock);t->sched_lock=false;}
static void thread_get_thread_name(thread_t t,char *n){snprintf(n,64,"%s",t->name);}
static uint64_t get_saved_state_pc(struct state *s){return s->pc;}
#define queue_iterate(head,t,type,field) for(t=*(head);t;t=t->next)
static void proc_iterate(unsigned flags,int(*fn)(proc_t,void*),void *arg,int(*filter)(proc_t,void*),void *farg){assert(flags==5);for(unsigned i=0;i<procCount;++i)if(filter(&procs[i],farg))assert(fn(&procs[i],arg)==0);}
'''
main=r'''
void boot_ui_trace_begin(void){}
static unsigned trace_calls[10];
void boot_ui_trace_task(task_t t,unsigned i,int p,const char *n,unsigned s){assert(!t->busy && interrupts && i>=1 && i<=4);++trace_calls[i];(void)p;(void)n;(void)s;}
void boot_ui_trace_publish(void){}
static void continuation(void){}
static void capture_and_snapshot(void){boot_process_capture();boot_process_snapshot();}
int main(void){
 boot_process_capture();assert(boot_process_passes==1&&boot_process_seen==0&&strstr(boot_process_rows[2],"absent"));
 struct state user={0x180000123};
 struct thread t[2]={{.state=TH_WAIT,.sched_pri=31,.wait_event=0x1234,.continuation=continuation,.machine={&user},.name="main"},{.state=TH_RUN,.name="worker"}};t[0].next=&t[1];
 struct task task={.threads=t,.thread_count=2,.task_shared_region_slide=0x10000};
 procs[0]=(struct proc){"backboardd",58,&task};procs[1]=(struct proc){"ignored",99,&task};procCount=2;
 boot_process_capture();assert(boot_process_seen==1&&interrupts&&!task.busy&&!t[0].sched_lock&&!t[1].sched_lock);
 assert(strstr(boot_process_rows[1],"n2 r1 w1 b0 t1 s1/31 W1234")&&strstr(boot_process_rows[1],"U180000123 +10000"));
 task.busy=true;boot_process_capture();assert(strstr(boot_process_rows[1],"busy")&&task.busy&&interrupts);task.busy=false;
 t[0].sched_lock=true;boot_process_capture();assert(strstr(boot_process_rows[1],"b1 t0")&&t[0].sched_lock&&interrupts);t[0].sched_lock=false;
 struct thread many[129]={0};for(unsigned i=0;i<129;++i){many[i].state=TH_RUN;many[i].name="worker";many[i].next=i<128?&many[i+1]:NULL;}
 task.threads=many;task.thread_count=129;boot_process_capture();assert(strstr(boot_process_rows[1],"n129 r128")&&interrupts&&!task.busy);
 procs[0]=(struct proc){"kernel_task",0,&task};procCount=1;many[1].name="VM_pageout_scan";many[1].state=TH_WAIT;many[1].wait_event=0x5678;
 boot_process_capture();assert(strstr(boot_process_rows[9],"t1 s1/0 W5678")&&strstr(boot_process_rows[1],"absent")&&interrupts&&!task.busy);
 chrono_focus_initialized=0;host_focus=1;procs[0]=(struct proc){"chronod",97,&task};procs[1]=(struct proc){"backboardd",58,&task};procCount=2;
 boot_process_capture();assert(strstr(capture_rows[1],"chronod p97")&&strstr(capture_rows[3],"backboardd p58"));
 chrono_focus_initialized=0;host_focus=0;strcpy(host_focus_name,"liveactivitiesd");strcpy(host_focus2_name,"coreduetd");procs[0]=(struct proc){"liveactivitiesd",101,&task};procs[1]=(struct proc){"coreduetd",77,&task};procCount=2;
 boot_process_capture();assert(strstr(capture_rows[1],"liveactivitiesd p101")&&!strcmp(targets[3],"backboardd")&&strstr(capture_rows[2],"coreduetd p77"));
 chrono_focus_initialized=0;host_focus=0;strcpy(host_focus_name,"configd");strcpy(host_focus2_name,"nehelper");strcpy(host_focus3_name,"profiled");strcpy(host_focus4_name,"SpringBoard");
 memset(trace_calls,0,sizeof(trace_calls));procs[0]=(struct proc){"configd",140,&task};procs[1]=(struct proc){"nehelper",110,&task};procs[2]=(struct proc){"profiled",122,&task};procs[3]=(struct proc){"SpringBoard",27,&task};procCount=4;
 boot_process_capture();assert(trace_calls[1]==1&&trace_calls[2]==1&&trace_calls[3]==1&&trace_calls[4]==1&&strstr(capture_rows[4],"SpringBoard p27")&&strstr(capture_rows[3],"profiled p122"));
 puts("PASS: actual process probe body; absent/present, task/thread busy, IRQ restoration, 128-thread bound, VM worker selection and fresh rows; kernel APIs substituted");
}
'''
main=main.replace('boot_process_capture();','capture_and_snapshot();')
# Keep the helper's actual production invocation rather than recursing.
main=main.replace('static void capture_and_snapshot(void){capture_and_snapshot();',
                  'static void capture_and_snapshot(void){boot_process_capture();')
(out/'test.c').write_text(prefix+body+main)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],check=True,timeout=60)
r=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,timeout=30)
(out/'report.json').write_text(json.dumps({'source_body_sha256':hashlib.sha256(body.encode()).hexdigest(),'passed':True,'kernel_apis_substituted':True,'hardware_observed':False},indent=2)+'\n');print(r.stdout,end='')
