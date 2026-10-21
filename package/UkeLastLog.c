// SPDX-License-Identifier: MIT
// Keep a persistent dump of the kernel message buffer in the reserved pram.
// The low half keeps the embedded-panic FUNK record that the next XNU boot
// prints; the high half is a ramoops-compatible console ring
// (persistent_ram_buffer, sig 0x43474244) that a reader can expose at
// /sys/fs/pstore/console-ramoops-0. Enabled with sheng-log=1. Only writes the
// pram mapping the device tree already reserved; it never allocates, changes
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

	if (gPanicBase == 0 || gPanicSize < UKE_RAMOOPS_OFFSET + UKE_RAMOOPS_SIZE) {
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

	if (gPanicBase == 0 || gPanicSize <= sizeof(struct embedded_panic_header) + 1) {
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

static void
uke_last_log_writer(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;

	for (;;) {
		uke_last_log_write();
		IOSleep(UKE_LAST_LOG_INTERVAL_MS);
	}
}

static void
uke_last_log_start(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("sheng-log", &enabled, (int)sizeof(enabled)) || enabled != 1) {
		return;
	}
	if (gPanicBase == 0 || gPanicSize <= sizeof(struct embedded_panic_header) + 1) {
		return;
	}
	if (kernel_thread_start_priority(uke_last_log_writer, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, uke_last_log_start);
