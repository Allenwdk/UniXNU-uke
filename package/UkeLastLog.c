// SPDX-License-Identifier: MIT
// Keep a persistent dump of the kernel message buffer in the reserved pram.
// The low half keeps the embedded-panic FUNK record that the next XNU boot
// prints; the high half is a ramoops-compatible console ring
// (persistent_ram_buffer, sig 0x43474244) that a reader can expose at
// /sys/fs/pstore/console-ramoops-0. Enabled with sheng-log=1. The pram half is
// the only thing written directly: the same snapshot is also appended to the
// userdata log window through NativeUfs, which is what makes it readable from
// recovery. Apart from those two the writer never allocates, changes
// translation, or touches a device register.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/appleapiopts.h>
#include <mach/vm_types.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/sched.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/msgbuf.h>
#include <IOKit/IOLib.h>

extern vm_offset_t gPanicBase;
extern unsigned int gPanicSize;
extern int desiredvnodes;
extern uint64_t sane_size;
extern void uke_crash_log_start(void);
#if defined(UKE_SOURCE_STORAGE_LOG)
extern volatile uint32_t boot_log_rounds;
extern volatile int32_t native_battery_stats[16];
extern volatile uint32_t native_battery_dispatch[6];
extern unsigned armcpu_stats_row(unsigned, char *, unsigned);
extern void sheng_cpufreq_poll(void);
extern char native_cpufreq_row[120];
extern volatile uint32_t native_cpufreq_ramp[9], native_gpu_dcvs_reasons[9], native_gpu_dcvs_windows[8];
extern volatile uint32_t native_gpu_pool_board[4], native_adreno_pool_state[12];
extern volatile uint64_t native_adreno_fence_wake[8], native_adreno_submit_us[8];
extern volatile uint64_t native_adreno_maint_walk[8];
extern volatile uint64_t native_adreno_map_met[7] __attribute__((weak_import));
extern volatile uint32_t native_adreno_sync_audit[6], native_adreno_sync_skips[2];
extern volatile uint32_t native_adreno_bo_owners[24][4], native_adreno_view_owners[24][4];
extern volatile uint32_t native_adreno_job_time[8], native_adreno_stall_kick[26], native_adreno_pool_views[5];
extern volatile uint32_t native_adreno_kernel_alloc_fail[6], native_adreno_switch_timeout[2][30], native_adreno_switch_timeouts;
extern uint32_t ml_outside_memory_pages, ml_outside_memory_refused;
extern char *PE_boot_args(void);
extern volatile uint64_t native_adreno_long_jobs[8][8], native_adreno_slow_jobs[16][8];
extern volatile uint32_t native_gpu_dcvs[15], native_gpu_dcvs_level_share[16];
extern uint32_t native_gpu_perf_vote[2];
extern volatile uint32_t sheng_edge_cross_type_steals[2] __attribute__((weak_import));
extern volatile uint32_t sheng_edge_idle_beside_excess[2] __attribute__((weak_import));
extern volatile uint32_t sheng_edge_idle_beside_queued[2][8] __attribute__((weak_import));
extern volatile int native_gpu_addon_released;
extern uint64_t ml_static_mfree_pages;

#else
extern void pe_print_last_panic_again(void);
#endif

#define UKE_LAST_LOG_INTERVAL_MS 500

// The 4 MiB pram region is split in two halves. The low half keeps the
// embedded-panic FUNK record for the XNU console; the high half is a Linux
// ramoops console zone (persistent_ram_buffer, no ECC), which the uke Linux
// ramoops backend exposes at /sys/fs/pstore/console-ramoops-0.
#define UKE_FUNK_LOG_SIZE   0x200000u
#define UKE_RAMOOPS_OFFSET  0x200000u
#define UKE_RAMOOPS_SIZE    0x200000u
#define UKE_RAMOOPS_MAGIC   0x43474244u /* "DBGC", see Linux pstore_ram.h */

struct uke_ramoops_buffer {
	uint32_t sig;
	uint32_t start;
	uint32_t size;
	uint8_t data[];
};

static uint32_t uke_ramoops_last_write;
static int uke_ramoops_initialized;
/* Set by the panic screen before it stages its text at gPanicBase; the two pram
 * writers below must leave that block alone once it is set. */
static volatile int ukePanicStaged;

static void
uke_ramoops_append(struct uke_ramoops_buffer *buffer, size_t capacity,
    const uint8_t *src, size_t count)
{
	size_t start, size, first;

	if (count == 0) {
		return;
	}
	if (count > capacity) {
		src += count - capacity;
		count = capacity;
	}
	start = buffer->start;
	size = buffer->size;
	if (start >= capacity) {
		start = 0;
	}
	if (size > capacity) {
		size = capacity;
	}
	first = capacity - start;
	if (first > count) {
		first = count;
	}
	memcpy(buffer->data + start, src, first);
	if (count > first) {
		memcpy(buffer->data, src + first, count - first);
	}
	size += count;
	if (size > capacity) {
		size = capacity;
	}
	start = (start + count) % capacity;
	// The Linux reader runs after a reset, but publish the bytes before the
	// header so a concurrent reader never sees a size that covers stale data.
	__asm__ volatile("dsb sy" ::: "memory");
	buffer->size = (uint32_t)size;
	buffer->start = (uint32_t)start;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
uke_ramoops_write(struct msgbuf *m)
{
	struct uke_ramoops_buffer *buffer;
	size_t capacity, size, start, first, tail;
	int write;

	if (gPanicBase == 0 || ukePanicStaged ||
	    gPanicSize < UKE_RAMOOPS_OFFSET + UKE_RAMOOPS_SIZE) {
		return;
	}
	buffer = (struct uke_ramoops_buffer *)(uintptr_t)(gPanicBase + UKE_RAMOOPS_OFFSET);
	capacity = UKE_RAMOOPS_SIZE - sizeof(*buffer);
	write = m->msg_bufx;
	if (write <= 0) {
		return;
	}
	size = (size_t)m->msg_size;
	if (!uke_ramoops_initialized) {
		buffer->sig = UKE_RAMOOPS_MAGIC;
		buffer->start = 0;
		buffer->size = 0;
		__asm__ volatile("dsb sy" ::: "memory");
		uke_ramoops_initialized = 1;
		// Seed the ring with the current msgbuf tail: the display/root work
		// runs before the first 500 ms tick, so a delta-only start would lose
		// the interesting beginning of the boot.
		tail = capacity;
		if (tail > size) {
			tail = size;
		}
		if (tail > (size_t)write) {
			tail = (size_t)write;
		}
		start = ((size_t)write + size - tail) % size;
		first = size - start;
		if (first > tail) {
			first = tail;
		}
		uke_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + start, first);
		if (tail > first) {
			uke_ramoops_append(buffer, capacity, (const uint8_t *)m->msg_bufc,
			    tail - first);
		}
		uke_ramoops_last_write = (uint32_t)write;
		return;
	}
	if ((uint32_t)write == uke_ramoops_last_write) {
		return;
	}
	if ((uint32_t)write > uke_ramoops_last_write) {
		uke_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + uke_ramoops_last_write,
		    (size_t)write - uke_ramoops_last_write);
	} else {
		uke_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + uke_ramoops_last_write,
		    size - uke_ramoops_last_write);
		uke_ramoops_append(buffer, capacity, (const uint8_t *)m->msg_bufc,
		    (size_t)write);
	}
	uke_ramoops_last_write = (uint32_t)write;
}

static void
uke_last_log_write(void)
{
	struct embedded_panic_header *hdr;
	struct msgbuf *m = msgbufp;
	size_t size, cap, start, first;
	int write;

	if (gPanicBase == 0 || ukePanicStaged ||
	    gPanicSize <= sizeof(struct embedded_panic_header) + 1) {
		return;
	}
	if (m == NULL || m->msg_bufc == NULL || m->msg_size <= 1 || m->msg_magic != MSG_MAGIC) {
		return;
	}
	write = m->msg_bufx;
	if (write <= 0) {
		return;
	}

	hdr = (struct embedded_panic_header *)(uintptr_t)gPanicBase;
	size = (size_t)m->msg_size;
	cap = UKE_FUNK_LOG_SIZE - sizeof(*hdr) - 1;
	if (cap > size) {
		cap = size;
	}

	// msg_bufr only advances when a reader drains the log, so it may point at
	// stale bytes after the ring wraps. The newest cap bytes before msg_bufx
	// are the ones a reset needs.
	start = ((size_t)write + size - cap) % size;
	first = size - start;
	if (first > cap) {
		first = cap;
	}
	memcpy((void *)(hdr + 1), m->msg_bufc + start, first);
	if (cap > first) {
		memcpy((char *)(hdr + 1) + first, m->msg_bufc, cap - first);
	}
	((char *)(hdr + 1))[cap] = '\0';

	hdr->eph_magic = EMBEDDED_PANIC_MAGIC;
	hdr->eph_crc = 0;
	hdr->eph_version = EMBEDDED_PANIC_HEADER_CURRENT_VERSION;
	hdr->eph_panic_flags = 0;
	hdr->eph_panic_log_offset = (uint32_t)sizeof(*hdr);
	hdr->eph_panic_log_len = (uint32_t)cap;
	hdr->eph_stackshot_offset = 0;
	hdr->eph_stackshot_len = 0;
	hdr->eph_other_log_offset = 0;
	hdr->eph_other_log_len = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	uke_ramoops_write(m);
}

// ---- userdata copy --------------------------------------------------------------------------
//
// The pram ring above survives only until the next reset and cannot be read
// from outside the device. The same snapshot is therefore also appended to the
// log window NativeUfs keeps at the end of userdata, which a recovery image or
// a host can read directly with dd.
//
// Layout: the window is a ring of 16 KiB records. Each record is
//
//   [ 0] magic "UKELLOG1"
//   [ 8] sequence number (u64, monotonic)
//   [16] payload length (u32)
//   [20] payload crc32 (u32)
//   [24] reserved
//   [32] payload (the newest bytes of the kernel message buffer)
//
// A reader sorts by sequence and concatenates the contiguous run it finds.
#define UKE_UFS_RECORD_BYTES   (16 * 1024)
#define UKE_UFS_RECORD_BLOCKS  (UKE_UFS_RECORD_BYTES / 4096)
#define UKE_UFS_PAYLOAD_BYTES  (UKE_UFS_RECORD_BYTES - 32)
#define UKE_UFS_INTERVAL_MS    500  /* diagnostic: keep the tail of a hang */

int native_ufs_log_open(uint64_t *firstLba, uint64_t *blocks, const char **why);
int native_ufs_log_write(uint64_t rel, uint32_t blocks, const void *src);
int native_ufs_log_sync(void);

static uint32_t
uke_crc32(const uint8_t *data, size_t length)
{
	uint32_t crc = 0xffffffffU;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
		}
	}
	return ~crc;
}

static uint8_t ukeUfsBuffer[UKE_UFS_RECORD_BYTES] __attribute__((aligned(4096)));
static bool ukeUfsOpen;
static bool ukeUfsRefused;
static uint64_t ukeUfsBlocks;
static uint64_t ukeUfsSeq;
static uint64_t ukeUfsCursor;

// One record: the newest UKE_UFS_PAYLOAD_BYTES of the message buffer.
static size_t
uke_snapshot(uint8_t *out, size_t capacity)
{
	struct msgbuf *m = msgbufp;
	if (m == NULL || m->msg_bufc == NULL || m->msg_size <= 1 || m->msg_magic != MSG_MAGIC) {
		return 0;
	}
	const int write = m->msg_bufx;
	if (write <= 0) {
		return 0;
	}
	size_t size = (size_t)m->msg_size;
	size_t cap = capacity;
	if (cap > size) {
		cap = size;
	}
	size_t start = ((size_t)write + size - cap) % size;
	size_t first = size - start;
	if (first > cap) {
		first = cap;
	}
	memcpy(out, m->msg_bufc + start, first);
	if (cap > first) {
		memcpy(out + first, m->msg_bufc, cap - first);
	}
	return cap;
}

static void
uke_ufs_publish(void)
{
	if (ukeUfsRefused) {
		return;
	}
	if (!ukeUfsOpen) {
		const char *why = "";
		uint64_t first = 0, blocks = 0;
		if (!native_ufs_log_open(&first, &blocks, &why)) {
			// "off" and "no root layout" are the normal states before the
			// storage driver has started; anything else will not change.
			if (why != NULL && strcmp(why, "off") != 0 &&
			    strcmp(why, "no root layout") != 0) {
				IOLog("UkeLastLog: userdata log window unavailable: %s\n", why);
				ukeUfsRefused = true;
			}
			return;
		}
		ukeUfsBlocks = blocks;
		ukeUfsCursor = 0;
		ukeUfsSeq = 0;
		ukeUfsOpen = true;
		IOLog("UkeLastLog: userdata log window at lba %llu, %llu blocks\n",
		    (unsigned long long)first, (unsigned long long)blocks);
	}

	bzero(ukeUfsBuffer, sizeof(ukeUfsBuffer));
	const size_t length = uke_snapshot(ukeUfsBuffer + 32, UKE_UFS_PAYLOAD_BYTES);
	if (length == 0) {
		return;
	}
	memcpy(ukeUfsBuffer, "UKELLOG1", 8);
	uint64_t *seq = (uint64_t *)(ukeUfsBuffer + 8);
	*seq = ukeUfsSeq;                       // little-endian AArch64
	uint32_t *len = (uint32_t *)(ukeUfsBuffer + 16);
	*len = (uint32_t)length;
	uint32_t *crc = (uint32_t *)(ukeUfsBuffer + 20);
	*crc = uke_crc32(ukeUfsBuffer + 32, length);
	__asm__ volatile("dsb sy" ::: "memory");

	if (!native_ufs_log_write(ukeUfsCursor, UKE_UFS_RECORD_BLOCKS, ukeUfsBuffer)) {
		IOLog("UkeLastLog: userdata log write failed at record %llu\n",
		    (unsigned long long)ukeUfsSeq);
		return;
	}
	(void)native_ufs_log_sync();
	ukeUfsSeq++;
#if defined(UKE_SOURCE_STORAGE_LOG)
	boot_log_rounds++;
#endif
	ukeUfsCursor = (ukeUfsCursor + UKE_UFS_RECORD_BLOCKS) % ukeUfsBlocks;
	ukeUfsCursor -= ukeUfsCursor % UKE_UFS_RECORD_BLOCKS;
}

/* Panic-time staging in the pram: [magic][length][text]. It is only a hand-off:
 * the next boot's writer copies the text into the userdata log window (disk). */
#define UKE_PANIC_SAVE_MAGIC 0x31564153u
static char ukePanicText[1024];
static boolean_t ukePanicTextValid = FALSE;

/* The writers run on other CPUs while the panic screen is parked; let a
 * snapshot that started before the staging flag retire before overwriting
 * the block it is about to keep. */
static void
uke_panic_settle(void)
{
	uint64_t start, now;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r" (start));
	do {
		__asm__ volatile("mrs %0, cntvct_el0" : "=r" (now));
	} while (now - start < 1000000);
}

void
uke_panic_flush(const char *message)
{
	static volatile int flushing;
	const char prefix[] = "UKE PANIC: ";
	size_t used = sizeof(prefix) - 1;
	if (flushing || message == NULL || gPanicBase == 0 || gPanicSize < 8 + used) {
		return;
	}
	flushing = 1;
	ukePanicStaged = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	uke_panic_settle();
	const size_t capacity = (size_t)gPanicSize - 8;
	size_t length = strlen(message);
	if (length > capacity - used) {
		length = capacity - used;
	}
	uint32_t *header = (uint32_t *)(uintptr_t)gPanicBase;
	memcpy((char *)(header + 2), prefix, used);
	memcpy((char *)(header + 2) + used, message, length);
	header[1] = (uint32_t)(used + length);
	__asm__ volatile("dsb sy" ::: "memory");
	header[0] = UKE_PANIC_SAVE_MAGIC;
	__asm__ volatile("dsb sy" ::: "memory");
	flushing = 0;
}

static void
uke_panic_restore(void)
{
	if (gPanicBase == 0 || gPanicSize < 8) {
		return;
	}
	uint32_t *header = (uint32_t *)(uintptr_t)gPanicBase;
	if (header[0] != UKE_PANIC_SAVE_MAGIC) {
		return;
	}
	const uint32_t length = header[1];
	if (length == 0 || (uint64_t)length > (uint64_t)gPanicSize - 8) {
		header[0] = 0;
		return;
	}
	const size_t copy = length < sizeof(ukePanicText) - 1 ? length : sizeof(ukePanicText) - 1;
	memcpy(ukePanicText, (const char *)(header + 2), copy);
	ukePanicText[copy] = '\0';
	ukePanicTextValid = TRUE;
	header[0] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
uke_last_log_writer(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;

	uke_panic_restore();
	if (ukePanicTextValid) {
		IOLog("%s\n", ukePanicText);
		ukePanicTextValid = FALSE;
	}

	unsigned tick = 0;
	for (;;) {
		// The first seconds of msgbuf are flooded by boot logs; report the
		// vnode sizing once the window is long past the ring's early noise.
		if (tick == 240) {
			IOLog("UkeLastLog: vnodes desired=%d sane=%llu\n",
			    desiredvnodes, (unsigned long long)sane_size);
			const char *args = PE_boot_args();
			const size_t length = args ? strnlen(args, 2048) : 0;
			uint32_t autoreset = 0;
			const int found = PE_parse_boot_argn("autoreset", &autoreset, sizeof(autoreset));
			const size_t tail = length > 80 ? 80 : length;
			IOLog("UkeLastLog: cmdline len=%lu autoreset=%d/%u tail=%.*s\n",
			    (unsigned long)length, found, autoreset, (int)tail, args ? args + length - tail : "");
			IOLog("UkeLastLog: outside memory adopted=%u refused=%u pages\n",
			    ml_outside_memory_pages, ml_outside_memory_refused);
		}
#if !defined(UKE_SOURCE_STORAGE_LOG)
		if (tick == 4) {
			/* Now that kprintf output reaches msgbuf, repeat the previous
			 * boot's panic text so it lands in the userdata window. */
			pe_print_last_panic_again();
		}
#endif
#if defined(UKE_SOURCE_STORAGE_LOG)
        /* Observe dispatch independently of the DSP and battery work-loop.
         * Counts are diagnostic samples, not a committed battery snapshot. */
        if ((tick % 40) == 0 && native_battery_stats[0] != 0) {
            uint32_t counts[6];
            for (unsigned i = 0; i < 6; ++i)
                counts[i] = __atomic_load_n(&native_battery_dispatch[i], __ATOMIC_RELAXED);
            IOLog("UKE BATTERY dispatch: st=%d state=%u rx=%u signal=%u returned=%u events=%u drain=%u ans=%d to=%x drop=%x refuse=%d\n",
                native_battery_stats[0], counts[0], counts[1], counts[2], counts[3], counts[4], counts[5],
                native_battery_stats[7], native_battery_stats[8], native_battery_stats[14], native_battery_stats[15]);
        }
        // Upstream's regular log round retries the L3 vote until firmware enables
        // that domain. Keep its existing one-shot guard; no new worker is needed.
        if ((tick % 4) == 0) sheng_cpufreq_poll();
        if ((tick % 40) == 0) {
            IOLog("UKE %s\n", native_cpufreq_row);
            char row[512];
            for (unsigned i = 0; i < 17 && armcpu_stats_row(i, row, sizeof row); ++i)
                IOLog("UKE %s\n", row);
            IOLog("UKE CPU ramp: full=%u/%u/%u late=%u/%u/%u statePct=%u/%u/%u\n",
                native_cpufreq_ramp[0], native_cpufreq_ramp[3], native_cpufreq_ramp[6],
                native_cpufreq_ramp[1], native_cpufreq_ramp[4], native_cpufreq_ramp[7],
                native_cpufreq_ramp[2], native_cpufreq_ramp[5], native_cpufreq_ramp[8]);
            IOLog("UKE DCVS windows: span=%u windows=%u busy=%u faint=%u ringOnly=%u longest=%uus@%u rests=%u\n",
                native_gpu_dcvs_windows[7], native_gpu_dcvs_windows[0], native_gpu_dcvs_windows[1],
                native_gpu_dcvs_windows[2], native_gpu_dcvs_windows[3], native_gpu_dcvs_windows[4],
                native_gpu_dcvs_windows[5], native_gpu_dcvs_windows[6]);
            IOLog("UKE DCVS reasons: %u/%u/%u/%u/%u/%u/%u/%u\n",
                native_gpu_dcvs_reasons[1], native_gpu_dcvs_reasons[2], native_gpu_dcvs_reasons[3], native_gpu_dcvs_reasons[4],
                native_gpu_dcvs_reasons[5], native_gpu_dcvs_reasons[6], native_gpu_dcvs_reasons[7], native_gpu_dcvs_reasons[8]);
            IOLog("UKE GPU fence wake: irq=%llu fence=%llu pass=%llu woken=%llu sleep=%llu timeout=%llu periodic=%llu switches=%llu\n",
                native_adreno_fence_wake[0], native_adreno_fence_wake[1], native_adreno_fence_wake[2], native_adreno_fence_wake[3],
                native_adreno_fence_wake[4], native_adreno_fence_wake[5], native_adreno_fence_wake[6], native_adreno_fence_wake[7]);
            IOLog("UKE GPU pool: board=%u/%u KiB=%u rc=%x ranges=%u total=%u free=%u alive=%u grantsKiB=%u retired=%u\n",
                native_gpu_pool_board[0], native_gpu_pool_board[1], native_gpu_pool_board[2], native_gpu_pool_board[3],
                native_adreno_pool_state[0], native_adreno_pool_state[1], native_adreno_pool_state[2],
                native_adreno_pool_state[4], native_adreno_pool_state[5], native_adreno_pool_state[8]);
            IOLog("UKE GPU governor: level=%u bw=%u changes=%u failed=%u busy=%u topShare=%u touch=%u wake=%u\n",
                native_gpu_perf_vote[0], native_gpu_perf_vote[1], native_gpu_dcvs[2], native_gpu_dcvs[3],
                native_gpu_dcvs[4], native_gpu_dcvs_level_share[9], native_gpu_dcvs[13], native_gpu_dcvs[14]);
            IOLog("UKE GPU pool views: hook=%u tracked=%u untracked=%u closes=%u returned=%u retiredKiB=%u\n",
                native_adreno_pool_views[4], native_adreno_pool_views[0], native_adreno_pool_views[1],
                native_adreno_pool_views[2], native_adreno_pool_views[3], native_adreno_pool_state[9]);
            IOLog("UKE boot memory: addonReleasedPages=%d staticFreedPages=%llu\n",
                native_gpu_addon_released, ml_static_mfree_pages);
            if (&sheng_edge_cross_type_steals && &sheng_edge_idle_beside_excess && &sheng_edge_idle_beside_queued) {
                IOLog("UKE CPU edges: steals=%u/%u idleExcess=%u/%u queuedE=%u/%u/%u/%u/%u/%u/%u/%u queuedP=%u/%u/%u/%u/%u/%u/%u/%u\n",
                    sheng_edge_cross_type_steals[0], sheng_edge_cross_type_steals[1],
                    sheng_edge_idle_beside_excess[0], sheng_edge_idle_beside_excess[1],
                    sheng_edge_idle_beside_queued[0][0], sheng_edge_idle_beside_queued[0][1],
                    sheng_edge_idle_beside_queued[0][2], sheng_edge_idle_beside_queued[0][3],
                    sheng_edge_idle_beside_queued[0][4], sheng_edge_idle_beside_queued[0][5],
                    sheng_edge_idle_beside_queued[0][6], sheng_edge_idle_beside_queued[0][7],
                    sheng_edge_idle_beside_queued[1][0], sheng_edge_idle_beside_queued[1][1],
                    sheng_edge_idle_beside_queued[1][2], sheng_edge_idle_beside_queued[1][3],
                    sheng_edge_idle_beside_queued[1][4], sheng_edge_idle_beside_queued[1][5],
                    sheng_edge_idle_beside_queued[1][6], sheng_edge_idle_beside_queued[1][7]);
            }
            if (native_adreno_job_time[0]) {
                const volatile uint32_t *t = native_adreno_job_time;
                IOLog("UKE GPU jobs: timed=%u stamped=%u over8=%u over16=%u over33=%u over100=%u longestUs=%u slow=%u\n",
                    t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
                for (unsigned i = 0; i < 6; ++i) {
                    const volatile uint64_t *j = i < 2 ? native_adreno_long_jobs[i] :
                        native_adreno_slow_jobs[(t[7] - 1 - (i - 2)) & 15];
                    if ((i >= 2 && i - 2 >= t[7]) || !j[1]) continue;
                    IOLog("UKE GPU job %s: fence=%u pid=%u q=%llx ring=%u pri=%d ibs=%u KiB=%llu us=%llu src=%u lvl=%u role=%u at=%llums lat=%lluus\n",
                        i < 2 ? "long" : "slow", (uint32_t)j[0], (uint32_t)j[3], j[4], (unsigned)(j[0] >> 40 & 15),
                        (int)(j[0] >> 44 & 0xff) - 1, (unsigned)(j[0] >> 32 & 0xff), j[2] >> 10, j[1],
                        (unsigned)(j[0] >> 52 & 15), (unsigned)(j[3] >> 40 & 0xffff), (unsigned)(j[3] >> 32 & 0xff),
                        j[5] / 1000, j[6]);
                }
            }
            if (native_adreno_stall_kick[25]) {
                const volatile uint32_t *k = native_adreno_stall_kick;
                IOLog("UKE GPU kick: n=%u fence=%u pendingUs=%u moved=%x retiredUs=%u pid=%u ibs=%u ring=%u pri=%d KiB=%u first=%x:%x/%u/%u at=%x:%x/%u/%u after=%x:%x/%u/%u\n",
                    k[25], k[0], k[1], k[20], k[21], k[22], k[23] & 0xffff, k[23] >> 16 & 0xff,
                    (int)(k[23] >> 24) - 1, k[24] >> 10,
                    k[3], k[2], k[6], k[7], k[9], k[8], k[12], k[13], k[15], k[14], k[18], k[19]);
            }
            if (&native_adreno_map_met && native_adreno_map_met[0]) {
                const volatile uint64_t *m = native_adreno_map_met;
                IOLog("UKE GPU map conflict: asked=%llx+%llx remap=%u occupant=%llx+%llx handle=%llx jobs=%llu kind=%llx\n",
                    m[0], m[1] & ~(1ull << 63), (unsigned)(m[1] >> 63), m[2], m[3], m[4], m[5], m[6]);
            }
            const volatile uint32_t *alloc = native_adreno_kernel_alloc_fail;
            if (alloc[5]) {
                IOLog("UKE GPU first allocation failure: stage=%u status=%x bytes=%u bits=%u contiguous=%u total=%u\n",
                    alloc[0], alloc[1], alloc[2], alloc[3], alloc[4], alloc[5]);
            }
            const unsigned timeouts = native_adreno_switch_timeouts;
            for (unsigned i = 0; i < 2 && i < timeouts; ++i) {
                const volatile uint32_t *words = native_adreno_switch_timeout[i];
                for (unsigned j = 0; j < 30; j += 6) {
                    IOLog("UKE GPU switch timeout %u/%u words %u: %08x %08x %08x %08x %08x %08x\n",
                        i + 1, timeouts, j, words[j], words[j + 1], words[j + 2], words[j + 3], words[j + 4], words[j + 5]);
                }
            }
            IOLog("UKE GPU maintenance: walk=%llu/%llu/%llu/%llu/%llu skip=%u/%u audit=%u dirty=%u submit=%llu totalUs=%llu\n",
                native_adreno_maint_walk[0], native_adreno_maint_walk[1], native_adreno_maint_walk[2],
                native_adreno_maint_walk[3], native_adreno_maint_walk[4], native_adreno_sync_skips[0], native_adreno_sync_skips[1],
                native_adreno_sync_audit[0], native_adreno_sync_audit[2], native_adreno_submit_us[0], native_adreno_submit_us[6]);
            for (unsigned i=0; i<24; ++i) {
                if (native_adreno_bo_owners[i][2])
                    IOLog("UKE GPU BO owner: pid=%u bytes=%u count=%u peak=%u\n", native_adreno_bo_owners[i][0],
                        native_adreno_bo_owners[i][1], native_adreno_bo_owners[i][2], native_adreno_bo_owners[i][3]);
                if (native_adreno_view_owners[i][2])
                    IOLog("UKE GPU view owner: pid=%u KiB=%u count=%u peak=%u\n", native_adreno_view_owners[i][0],
                        native_adreno_view_owners[i][1], native_adreno_view_owners[i][2], native_adreno_view_owners[i][3]);
            }
        }

#endif
		uke_last_log_write();
		// The userdata copy is paced separately: the 32 MiB window only holds
		// 2048 records, so at the pram writer's 500 ms rate it would wrap every
		// seventeen minutes and discard the start of the boot, which is what
		// this copy exists to keep. Two seconds stretches the ring to about an
		// hour.
		if ((tick++ % (UKE_UFS_INTERVAL_MS / UKE_LAST_LOG_INTERVAL_MS)) == 0) {
			uke_ufs_publish();
		}
		IOSleep(UKE_LAST_LOG_INTERVAL_MS);
	}
}

static void
uke_last_log_start(void)
{
	extern void uke_gpu_export_start(void);
	uke_gpu_export_start();
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("sheng-log", &enabled, (int)sizeof(enabled)) || enabled != 1) {
		return;
	}
	if (kernel_thread_start_priority(uke_last_log_writer, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, uke_last_log_start);
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, uke_crash_log_start);
