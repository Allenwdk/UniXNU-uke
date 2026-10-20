#!/usr/bin/env python3
"""Block process traversal and compare actual old/new monitor bodies on host.

Kernel traversal, scheduling, clock and pixel dependencies are stand-ins.
This tests fault containment, not whether the physical device hit this fault.
"""
import ast
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build/cpu-desktop/monitor-isolation'
out.mkdir(parents=True, exist_ok=True)
tree = ast.parse((root / 'scripts/check-boot-process-probe.py').read_text())
prefix = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
              and any(isinstance(t, ast.Name) and t.id == 'prefix' for t in n.targets))
prefix = prefix.replace('static void proc_iterate(', 'static void before_iterate(void);\nstatic void proc_iterate(')
prefix = prefix.replace('{assert(flags==5);', '{before_iterate();assert(flags==5);')

support = r'''
#include <pthread.h>
#include <stdatomic.h>
typedef int wait_result_t;
typedef void *event_t;
#define THREAD_UNINT 0
#define NSEC_PER_MSEC 1000000
#define THREAD_CONTINUE_NULL 0
static unsigned long long thread_count;
static void (*boot_ui_trace_snapshot)(unsigned) __attribute__((unused));
#if FIXED
void boot_ui_trace_begin(void){}
void boot_ui_trace_task(task_t t,unsigned i,int p,const char *n,unsigned s){(void)t;(void)i;(void)p;(void)n;(void)s;}
void boot_ui_trace_publish(void){}
#endif
static unsigned frames,clock_seconds=73;
static int nprocs=1;
static unsigned process_capture_age;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool hold,entered;
static void before_iterate(void){
 assert(!pthread_mutex_lock(&gate));
 if(hold){entered=true;assert(!pthread_cond_broadcast(&changed));}
 while(hold)assert(!pthread_cond_wait(&changed,&gate));
 assert(!pthread_mutex_unlock(&gate));
}
static void wait_for_capture(void){
 assert(!pthread_mutex_lock(&gate));
 while(!entered)assert(!pthread_cond_wait(&changed,&gate));
 assert(!pthread_mutex_unlock(&gate));
}
static void release_capture(void){
 assert(!pthread_mutex_lock(&gate));hold=false;
 assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&gate));
}
static unsigned seconds(void){return clock_seconds;}
static void boot_progress_poll(void){++clock_seconds;}
static thread_t current_thread(void){return NULL;}
static void thread_set_thread_name(thread_t t,const char *n){(void)t;assert(!strcmp(n,"boot_progress"));}
static void refresh(unsigned now){assert(now==clock_seconds);++frames;}
static void assert_wait_timeout(event_t e,int mode,int count,int unit){
 assert(e==&thread_count && mode==THREAD_UNINT && count==1000 && unit==NSEC_PER_MSEC);
}
static void thread_block(int c){static unsigned rounds;assert(c==THREAD_CONTINUE_NULL);if(++rounds==8)pthread_exit(NULL);}
'''
main = r'''
static void *run_monitor(void *arg){(void)arg;monitor(NULL,0);return NULL;}
#if FIXED
static void *run_capture(void *arg){(void)arg;boot_process_capture();return NULL;}
#endif
int main(void){
 struct thread thread={.state=TH_WAIT,.name="main"};
 struct task task={.threads=&thread,.thread_count=1};
 boot_process_capture();
#if FIXED
 boot_process_snapshot();
#endif
 char previous[sizeof(boot_process_rows)];memcpy(previous,boot_process_rows,sizeof(previous));
 procs[0]=(struct proc){"backboardd",58,&task};procCount=1;
 pthread_t observer;
 hold=true;
#if FIXED
 pthread_t worker;assert(!pthread_create(&worker,NULL,run_capture,NULL));
 wait_for_capture();
 unsigned attempts=__atomic_load_n(&boot_process_attempts,__ATOMIC_RELAXED);
 boot_process_capture(); // A second caller cannot corrupt the blocked pass.
 assert(__atomic_load_n(&boot_process_attempts,__ATOMIC_RELAXED)==attempts);
 assert(!pthread_create(&observer,NULL,run_monitor,NULL));
 assert(!pthread_join(observer,NULL));
 assert(frames==8 && thread_count==8 && clock_seconds==81 && process_capture_age==7);
 assert(__atomic_load_n(&boot_process_busy,__ATOMIC_ACQUIRE)==1);
 assert(!memcmp(previous,boot_process_rows,sizeof(previous)) && boot_process_passes==1);
 release_capture();assert(!pthread_join(worker,NULL));
 boot_process_snapshot();
 assert(!boot_process_busy && boot_process_passes==2 && boot_process_seen==1);
 assert(strstr(boot_process_rows[1],"backboardd p58 n1"));
 memcpy(previous,boot_process_rows,sizeof(previous));
 publish_lock=1;boot_process_snapshot();assert(!memcmp(previous,boot_process_rows,sizeof(previous)));
 boot_process_capture(); // Publication also refuses contention without waiting.
 assert(boot_process_passes==2 && !memcmp(previous,boot_process_rows,sizeof(previous)));
 publish_lock=0;boot_process_capture();boot_process_snapshot();assert(boot_process_passes==4);
 puts("PASS fixed: eight monitor refreshes and advancing capture age during blocked traversal; complete snapshots, reentry and publish contention safe");
#else
 assert(!pthread_create(&observer,NULL,run_monitor,NULL));wait_for_capture();
 assert(frames==0 && thread_count==1 && clock_seconds==74);
 release_capture();assert(!pthread_join(observer,NULL));assert(frames==8);
 (void)process_capture_age;
 puts("PASS baseline: actual old monitor cannot refresh while its process traversal is blocked; release restores refresh");
#endif
}
'''
results = []
for fixed in (False, True):
    def source(name):
        if fixed:
            return (root / name).read_text()
        return subprocess.check_output(['git', 'show', '981989d:' + name], cwd=root, text=True, timeout=10)
    probe = source('kits/uke/BootProcessProbe.c')
    probe = probe[probe.index('char boot_process_rows'):]
    progress = source('kits/uke/BootProgress.c')
    monitor = progress[progress.index('static void monitor('):progress.index('\nvoid boot_progress_start(')]
    c = out / ('fixed.c' if fixed else 'baseline.c')
    exe = c.with_suffix('')
    c.write_text(prefix + probe + support + monitor + main)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-pointer-bool-conversion', '-O1', '-g', '-pthread',
                    '-fsanitize=address,undefined', '-DFIXED=' + str(int(fixed)), str(c), '-o', str(exe)],
                   check=True, timeout=30)
    result = subprocess.run([str(exe)], check=True, text=True, capture_output=True, timeout=10)
    print(result.stdout.strip())
    results.append({'fixed': fixed, 'probe_body_sha256': hashlib.sha256(probe.encode()).hexdigest(),
                    'monitor_body_sha256': hashlib.sha256(monitor.encode()).hexdigest(),
                    'result': result.stdout.strip()})
(out / 'report.json').write_text(json.dumps({'results': results, 'kernel_apis_substituted': True,
    'original_proc_iterate_executed': False, 'hardware_trigger_proven': False,
    'vm_executed': False, 'device_operation': False}, indent=2) + '\n')
