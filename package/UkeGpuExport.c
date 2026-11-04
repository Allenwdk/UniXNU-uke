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
extern void boot_diag_shot_tick(unsigned, unsigned);
extern uint64_t boot_diag_scan(uint64_t);
extern int boot_diag_stream(int (*)(const void *,uint32_t,void *),void *);
struct uke_diag_sink {uint8_t *stage;uint32_t target,filled,limit;};
static int uke_diag_flush(struct uke_diag_sink *k) {
 const uint32_t stage=LogExportStage*LogBlock,used=k->filled%stage?k->filled%stage:stage;
 const uint32_t blocks=(used+LogBlock-1)/LogBlock;
 if(used%LogBlock)memset(k->stage+used,0,blocks*LogBlock-used);
 return native_ufs_export_write(k->target+(k->filled-used)/LogBlock,blocks,k->stage)?0:-6;
}
static int uke_diag_emit(const void *data,uint32_t size,void *arg) {
 struct uke_diag_sink *k=arg;const uint8_t *p=data;const uint32_t stage=LogExportStage*LogBlock;
 if(k->filled>k->limit || size>k->limit-k->filled)return -5;
 while(size){uint32_t at=k->filled%stage,n=size<stage-at?size:stage-at;memcpy(k->stage+at,p,n);k->filled+=n;p+=n;size-=n;if(at+n==stage && uke_diag_flush(k))return -6;}
 return 0;
}
static void uke_diag_worker(void *arg,wait_result_t wait) {
 const unsigned diag=(unsigned)(uintptr_t)arg;
 (void)wait;unsigned elapsed=0;int root_failure_noted=0;
 for(;;){IOSleep(1000);++elapsed;boot_diag_shot_tick(elapsed,diag);
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
  if(elapsed<240 || !native_ufs_export_blocks())continue;
  IOLockLock(export_lock);const uint32_t region=(uint32_t)native_ufs_export_blocks(),half=region/2;
  struct uke_diag_sink k={NULL,export_ready && export_ready_first==0?half:0,0,half*LogBlock};diag_owned=1;IOLockUnlock(export_lock);
  int status=-4;uint64_t stream=0;
  if(half && (k.stage=IOMallocAligned(LogExportStage*LogBlock,LogBlock))) {
   stream=boot_diag_scan((uint64_t)half*LogBlock-16);
   if(stream && stream<=UINT32_MAX-16){uint32_t head[2]={(uint32_t)stream,0};status=uke_diag_emit("SHEXR001",8,&k);
    if(!status)status=uke_diag_emit(head,8,&k);if(!status)status=boot_diag_stream(uke_diag_emit,&k);
    if(!status && k.filled%(LogExportStage*LogBlock))status=uke_diag_flush(&k);
    if(!status && k.filled!=stream+16)status=-9;
    if(!status && !native_ufs_log_sync())status=-8;
   }else status=-7;
   IOFreeAligned(k.stage,LogExportStage*LogBlock);
  }
  IOLog("UKE DIAG EXPORT: status=%d first=%u bytes=%u region=%u\n",status,k.target,k.filled,region);
  return;
 }
}
void uke_gpu_export_start(void) {
 export_lock=IOLockAlloc();
 unsigned diag=0;
 if(!export_lock || !PE_parse_boot_argn("sheng-diag",&diag,sizeof diag) || (diag!=1 && diag!=2))return;
 thread_t t=THREAD_NULL;
 if(kernel_thread_start(uke_diag_worker,(void *)(uintptr_t)diag,&t)==KERN_SUCCESS)thread_deallocate(t);
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

