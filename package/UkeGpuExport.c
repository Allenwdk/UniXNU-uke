/* SPDX-License-Identifier: MIT */
/* Upstream BootLog export transport, using uke's existing text log for its
 * completed-blob descriptor. No changes to the bounded UFS export window. */
#include <IOKit/IOLib.h>
#include <stdint.h>
#include <string.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
extern uint64_t native_ufs_export_blocks(void);
extern int native_ufs_export_write(uint64_t,uint32_t,const void *);
extern int native_ufs_log_sync(void);
extern const char *volatile native_ufs_root_failure __attribute__((weak_import));
extern void native_ufs_progress_text(char *,size_t) __attribute__((weak_import));
extern void boot_root_thread_snapshot(void) __attribute__((weak_import));
extern void uke_vm_dw_pool_snapshot(uint64_t *) __attribute__((weak_import));
extern uint32_t native_adreno_preflight_ready __attribute__((weak_import));
extern uint32_t native_adreno_preflight_entries __attribute__((weak_import));
enum {LogBlock=4096,LogExportStage=16};
static IOLock *export_lock;
static uint8_t *export_stage;
static uint32_t export_region,export_total,export_filled,export_generation,export_target;
static uint32_t export_ready,export_ready_bytes,export_ready_first;
static int diag_owned;
char boot_log_autoreset_text[200];
static thread_t flight_writer_thread,diag_writer_thread;
static uint64_t flight_progress,diag_progress;
static unsigned flight_phase,diag_phase;
extern int boot_flight_worker_stack(thread_t,uint64_t *,unsigned *,uint64_t *,uint64_t [12]);

static uint64_t flight_us(void){
 uint64_t ns=0;
 absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
 return ns/1000;
}

static void trace_worker_progress(uint64_t *progress,unsigned *phase,unsigned value){
 __atomic_store_n(phase,value,__ATOMIC_RELAXED);
 __atomic_store_n(progress,flight_us(),__ATOMIC_RELEASE);
}

static void trace_report_worker(const char *name,thread_t *worker,uint64_t *progress,unsigned *phase){
 const thread_t thread=__atomic_load_n(worker,__ATOMIC_ACQUIRE);
 const uint64_t last=__atomic_load_n(progress,__ATOMIC_ACQUIRE);
 const uint64_t now=flight_us();
 if(!thread || !last || now<last || now-last<10000000)return;
 uint64_t tid=0,event=0,frames[12]={0};
 unsigned state=0;
 int locked=boot_flight_worker_stack(thread,&tid,&state,&event,frames);
 IOLog("UKE trace stalled: %s ageUs=%llu phase=%u tid=%llu locked=%d state=%x wait=%llx\n",
     name,now-last,__atomic_load_n(phase,__ATOMIC_RELAXED),tid,locked,state,event);
 if(locked)IOLog("UKE trace stack: %s %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx\n",
     name,frames[0],frames[1],frames[2],frames[3],frames[4],frames[5],frames[6],frames[7],
     frames[8],frames[9],frames[10],frames[11]);
}

void uke_trace_worker_status(void){
 trace_report_worker("frame-flight",&flight_writer_thread,&flight_progress,&flight_phase);
 trace_report_worker("frame-export",&diag_writer_thread,&diag_progress,&diag_phase);
}
extern void boot_diag_shot_tick(unsigned, unsigned);
extern uint64_t boot_diag_scan(uint64_t);
extern int boot_diag_stream(int (*)(const void *,uint32_t,void *),void *);
struct uke_diag_sink {uint8_t *stage,*head;uint32_t target,filled,limit;};
static int uke_diag_flush(struct uke_diag_sink *k) {
 const uint32_t stage=LogExportStage*LogBlock,used=k->filled%stage?k->filled%stage:stage;
 const uint32_t blocks=(used+LogBlock-1)/LogBlock;
 if(used%LogBlock)memset(k->stage+used,0,blocks*LogBlock-used);
 const uint32_t first=(k->filled-used)/LogBlock;
 if(first==0)memcpy(k->head,k->stage,LogBlock);
 const uint32_t skip=first==0;
 if(blocks==skip)return 0;
 return native_ufs_export_write(k->target+first+skip,blocks-skip,k->stage+skip*LogBlock)?0:-6;
}
static int uke_diag_emit(const void *data,uint32_t size,void *arg) {
 struct uke_diag_sink *k=arg;const uint8_t *p=data;const uint32_t stage=LogExportStage*LogBlock;
 if(k->filled>k->limit || size>k->limit-k->filled)return -5;
 while(size){uint32_t at=k->filled%stage,n=size<stage-at?size:stage-at;memcpy(k->stage+at,p,n);k->filled+=n;p+=n;size-=n;if(at+n==stage && uke_diag_flush(k))return -6;}
 return 0;
}
static void uke_diag_worker(void *arg,wait_result_t wait) {
 const unsigned diag=(unsigned)(uintptr_t)arg;
 unsigned next=diag==4?180u:240u,repeat=0;
 PE_parse_boot_argn("sheng-diag-export",&next,sizeof next);
 PE_parse_boot_argn("uke-diag-repeat",&repeat,sizeof repeat);
 if(!next)return;
 (void)wait;unsigned elapsed=0;int root_failure_noted=0;
 for(;;){
  trace_worker_progress(&diag_progress,&diag_phase,0);
  IOSleep(1000);
  uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);elapsed=(unsigned)(ns/1000000000ull);
  trace_worker_progress(&diag_progress,&diag_phase,1);
  boot_diag_shot_tick(elapsed,diag);
  if(!root_failure_noted && &native_ufs_root_failure && native_ufs_root_failure) {
   root_failure_noted=1;
   IOLog("UKE ROOT FAILURE: %s preflight-ready=%u entries=%u\n",native_ufs_root_failure,
     &native_adreno_preflight_ready?__atomic_load_n(&native_adreno_preflight_ready,__ATOMIC_ACQUIRE):0u,
     &native_adreno_preflight_entries?__atomic_load_n(&native_adreno_preflight_entries,__ATOMIC_RELAXED):0u);
   if(&native_ufs_progress_text) {char snapshot[2048];native_ufs_progress_text(snapshot,sizeof snapshot);IOLog("UKE ROOT CACHED: %s\n",snapshot);}
   if(&uke_vm_dw_pool_snapshot) {
    uint64_t pool[4]={};uke_vm_dw_pool_snapshot(pool);
    IOLog("UKE ROOT DWPOOL: used=%llx gets=%llu puts=%llu unavailable=%llu\n",pool[0],pool[1],pool[2],pool[3]);
   }
   if(&boot_root_thread_snapshot)boot_root_thread_snapshot();
  }
  if(elapsed<next || !native_ufs_export_blocks())continue;
  trace_worker_progress(&diag_progress,&diag_phase,2);
  IOLockLock(export_lock);const uint32_t region=(uint32_t)native_ufs_export_blocks(),half=region/2;
  struct uke_diag_sink k={NULL,NULL,export_ready && export_ready_first==0?half:0,0,half*LogBlock};diag_owned=1;IOLockUnlock(export_lock);
  int status=-4;uint64_t stream=0;
  if(half && (k.stage=IOMallocAligned((LogExportStage+1)*LogBlock,LogBlock))) {
   k.head=k.stage+LogExportStage*LogBlock;
   trace_worker_progress(&diag_progress,&diag_phase,3);
   stream=boot_diag_scan((uint64_t)half*LogBlock-16);
   if(stream && stream<=UINT32_MAX-16){uint32_t head[2]={(uint32_t)stream,elapsed};
    // Invalidate the target before changing its body. Leave the other half readable.
    trace_worker_progress(&diag_progress,&diag_phase,4);
    memset(k.head,0,LogBlock);
    status=native_ufs_export_write(k.target,1,k.head) && native_ufs_log_sync()?0:-8;
    if(!status)status=uke_diag_emit("SHEXR001",8,&k);
    if(!status)status=uke_diag_emit(head,8,&k);
    trace_worker_progress(&diag_progress,&diag_phase,5);
    if(!status)status=boot_diag_stream(uke_diag_emit,&k);
    if(!status && k.filled%(LogExportStage*LogBlock))status=uke_diag_flush(&k);
    if(!status && k.filled!=stream+16)status=-9;
    trace_worker_progress(&diag_progress,&diag_phase,6);
    if(!status && !native_ufs_log_sync())status=-8;
    if(!status && (!native_ufs_export_write(k.target,1,k.head) || !native_ufs_log_sync()))status=-8;
   }else status=-7;
   IOFreeAligned(k.stage,(LogExportStage+1)*LogBlock);
  }
  if(!status) {
   IOLockLock(export_lock);
   export_ready=1;export_ready_bytes=k.filled;export_ready_first=k.target;
   IOLockUnlock(export_lock);
  }
  IOLog("UKE DIAG EXPORT: status=%d first=%u bytes=%u region=%u uptime=%u\n",status,k.target,k.filled,region,elapsed);
  if(!repeat){trace_worker_progress(&diag_progress,&diag_phase,7);__atomic_store_n(&diag_progress,0,__ATOMIC_RELEASE);return;}
  absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
  next=(unsigned)(ns/1000000000ull)+repeat;
 }
}
#include "UkeFlightRecorder.h"
void uke_gpu_export_start(void) {
 flight_start();
 export_lock=IOLockAlloc();
 unsigned diag=0;
 if(!export_lock || !PE_parse_boot_argn("sheng-diag",&diag,sizeof diag) || (diag!=1 && diag!=2 && diag!=4))return;
 thread_t t=THREAD_NULL;
 // Retain the worker so the independent text logger can sample a stale wait.
 if(kernel_thread_start(uke_diag_worker,(void *)(uintptr_t)diag,&t)==KERN_SUCCESS)
  __atomic_store_n(&diag_writer_thread,t,__ATOMIC_RELEASE);
}
int boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset, const void *data, uint32_t size) {
  if (!export_lock || !generation || !total || !data || !size || offset > total || size > total - offset) return -1;
  IOLockLock(export_lock);
  export_region = (uint32_t)native_ufs_export_blocks();
  int status = 0;
  const uint32_t half = export_region / 2, stage = LogExportStage * LogBlock;
  if (!half || !&native_ufs_export_write) status = -4;
  else if (total > (uint64_t)half * LogBlock) status = -5;
  else if (diag_owned) status = -7;
  else if (export_ready & 0x80000000u) status = -10;   // a generation with the top bit set (a device's RAM dump) is kept
  if (!status && !export_stage) export_stage = (uint8_t *)IOMallocAligned(stage, LogBlock);
  if (!status && !export_stage) status = -2;
  if (!status && offset == 0) {
    export_generation = generation; export_total = total; export_filled = 0;
    export_target = export_ready && export_ready_first == 0 ? half : 0;
  }
  if (!status && (generation != export_generation || total != export_total || offset != export_filled)) status = -3;   // only in order
  const uint8_t *from = (const uint8_t *)data;
  while (!status && size) {
    const uint32_t at = export_filled % stage, n = size < stage - at ? size : stage - at;
    memcpy(export_stage + at, from, n);
    export_filled += n; from += n; size -= n;
    const int full = at + n == stage, done = export_filled == export_total;
    if (full || done) {
      const uint32_t used = at + n, blocks = (used + LogBlock - 1) / LogBlock;
      if (used % LogBlock) memset(export_stage + used, 0, blocks * LogBlock - used);
      if (!native_ufs_export_write(export_target + (export_filled - used) / LogBlock, blocks, export_stage)) {
        status = -6; export_generation = 0;   // the next generation starts over
      }
    }
    if (!status && done) {export_ready = export_generation; export_ready_bytes = export_total; export_ready_first = export_target;
      if (!native_ufs_log_sync()) status = -8;
      else IOLog("UKE GPU EXPORT: generation=%u bytes=%u first=%u region=%u\n", export_ready, export_ready_bytes, export_ready_first, export_region);}
  }
  IOLockUnlock(export_lock);
  return status;
}

