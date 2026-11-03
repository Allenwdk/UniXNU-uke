/* SPDX-License-Identifier: MIT */
/* Upstream BootLog export transport, using uke's existing text log for its
 * completed-blob descriptor. No changes to the bounded UFS export window. */
#include <IOKit/IOLib.h>
#include <stdint.h>
#include <string.h>
extern uint64_t native_ufs_export_blocks(void);
extern int native_ufs_export_write(uint64_t,uint32_t,const void *);
extern int native_ufs_log_sync(void);
enum {LogBlock=4096,LogExportStage=16};
static IOLock *export_lock;
static uint8_t *export_stage;
static uint32_t export_region,export_total,export_filled,export_generation,export_target;
static uint32_t export_ready,export_ready_bytes,export_ready_first;
static const int diag_owned=0;
void uke_gpu_export_start(void) {export_lock=IOLockAlloc();}
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

