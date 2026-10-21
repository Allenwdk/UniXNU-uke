// SPDX-License-Identifier: MIT
// Keep the tail of the kernel message buffer in the reserved panic region so
// the next boot can print what the previous one logged before it reset.
// Enabled with sheng-log=1. Only writes the pram mapping that the device tree
// already reserved; it never allocates, changes translation, or touches a
// device register.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
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
	cap = gPanicSize - sizeof(*hdr) - 1;
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
