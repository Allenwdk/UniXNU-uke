/* SPDX-License-Identifier: MIT */
/* Independent of VFS and of faultable user-stack walks. NativeUfs's log write
 * uses FUA, within the upper 16 MiB of the existing 32 MiB log reservation. */
#include "UkeFlightStore.h"
#include "BootLatencyTrace.h"
extern char bootsessionuuid_string[37];
extern int native_ufs_log_write(uint64_t,uint32_t,const void *);
extern uint32_t native_gpu_trace_since(uint64_t *,void *,uint32_t,uint64_t *) __attribute__((weak_import));
extern void boot_flight_probe_start(void);
extern uint64_t boot_flight_probe_status[6];
extern void boot_flight_cpu_status(uint64_t [32]);
extern void boot_flight_sample_stale_workers(uint64_t);
extern volatile uint64_t native_adreno_submit_us[8],native_adreno_fence_wake[8];
extern volatile uint64_t native_data_io_reads,native_data_io_writes,native_data_flushes,native_data_persist_errors;
extern volatile uint32_t native_legacy_swapstat[16],native_adreno_stall_hang,native_adreno_global_fault[4],native_adreno_recover_trace[4];
extern volatile uint32_t native_gpu_dcvs[15];
extern uint32_t native_gpu_perf_vote[2];
static int flight_write(uint64_t first,uint32_t blocks,const void *data,void *context){
 (void)context;
 trace_worker_progress(&flight_progress,&flight_phase,3);
 const int result=native_ufs_log_write(first,blocks,data);
 trace_worker_progress(&flight_progress,&flight_phase,2);
 return result;
}
static void flight_worker(void *arg,wait_result_t wait) {
 (void)arg;(void)wait;thread_set_thread_name(current_thread(),"frame-flight");
 uint8_t *memory=IOMallocAligned(2*FlSlotBytes,FlBlock);
 if(!memory){IOLog("UKE FLIGHT: allocation failed\n");return;}
 memset(memory,0,2*FlSlotBytes);
 struct fl_writer writer={0};writer.buffer=memory;writer.used=FlHeaderBytes;writer.write=flight_write;
 uint64_t cursor[3]={0},lost[3]={0},errors=0,ticks=0;
 uint8_t *scratch=memory+FlSlotBytes;
 while(!native_ufs_export_blocks() || !bootsessionuuid_string[0])IOSleep(250);
 trace_worker_progress(&flight_progress,&flight_phase,1);
 memcpy(writer.boot,bootsessionuuid_string,36);writer.started_us=flight_us();
 boot_latency_start();boot_flight_probe_start();
 IOLog("UKE FLIGHT: start=%llu boot=%s first=%u blocks=%u tick-ms=250 FUA=1\n",writer.started_us,writer.boot,FlFirstBlock,FlRegionBlocks);
 for(;;){
  trace_worker_progress(&flight_progress,&flight_phase,2);
  const uint64_t now=flight_us();
  /* A previous failed FUA keeps its staged bytes and retries before reading
   * more. Source cursors advance only after their bytes enter this buffer. */
  if(writer.records && !fl_flush(&writer,now)){++errors;IOSleep(250);continue;}
  uint64_t beat[32]={now,++ticks,cursor[0],cursor[1],cursor[2],lost[0],lost[1],lost[2],errors,
   native_adreno_submit_us[0],native_adreno_fence_wake[1],native_legacy_swapstat[0],native_legacy_swapstat[9],native_legacy_swapstat[10],native_legacy_swapstat[11],
   native_data_io_reads,native_data_io_writes,native_data_flushes,native_data_persist_errors,
   native_adreno_stall_hang,native_adreno_global_fault[0],native_adreno_recover_trace[0],native_gpu_perf_vote[0],native_gpu_dcvs[4],
   (uintptr_t)&flight_worker,0,0,0,0,0,0,0};
  for(unsigned i=0;i<6;++i)beat[25+i]=__atomic_load_n(&boot_flight_probe_status[i],__ATOMIC_RELAXED);
  if(!fl_emit(&writer,4,ticks,beat,sizeof beat,now)){++errors;IOSleep(250);continue;}
  uint64_t cpus[32]={0};boot_flight_cpu_status(cpus);
  if(!fl_emit(&writer,5,ticks,cpus,sizeof cpus,now)){++errors;IOSleep(250);continue;}
  if(ticks%4==0)boot_flight_sample_stale_workers(now);
  /* Per-source quotas prevent a busy IPC producer from starving GPU events,
   * critical faults, the independent probe or the writer heartbeat. */
  for(unsigned source=0;source<3;++source)for(unsigned turn=0;turn<8;++turn) {
   uint64_t next=cursor[source],missing=lost[source];uint32_t bytes=0;
   if(source==0){if(&native_gpu_trace_since)bytes=native_gpu_trace_since(&next,scratch,32760,&missing);}
   else bytes=boot_latency_flight_read(source-1,&next,scratch,32760,&missing);
   if(bytes && !fl_emit(&writer,source+1,next,scratch,bytes,flight_us())){++errors;break;}
   cursor[source]=next;lost[source]=missing;
   if(bytes<32700)break;
  }
  if(!fl_flush(&writer,flight_us()))++errors;
  if(ticks%40==0)IOLog("UKE FLIGHT: slots=%llu us=%llu dropped=%llu/%llu/%llu errors=%llu\n",writer.sequence,writer.last_us,lost[0],lost[1],lost[2],errors);
  trace_worker_progress(&flight_progress,&flight_phase,0);
  IOSleep(250);
 }
}
static void flight_start(void) {
 unsigned enabled=0;if(!PE_parse_boot_argn("uke-flight",&enabled,sizeof enabled) || enabled!=1)return;
 thread_t thread=THREAD_NULL;
 if(kernel_thread_start(flight_worker,NULL,&thread)==KERN_SUCCESS)
  __atomic_store_n(&flight_writer_thread,thread,__ATOMIC_RELEASE);
}
