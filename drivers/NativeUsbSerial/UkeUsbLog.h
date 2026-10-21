// SPDX-License-Identifier: BSD-2-Clause-Patent
// Cursor logic for the kernel msgbuf ring. The kernel side takes
// bsd_log_lock_safe() around the copy; this header is pure and host-testable.
#ifndef UKE_USB_LOG_H
#define UKE_USB_LOG_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct UkeUsbLogCursor {
	uint32_t size;
	uint32_t cursor;
	bool primed;

	void prime(uint32_t ringSize, uint32_t write, bool full)
	{
		size = ringSize;
		cursor = full ? 0U : write;
		primed = true;
	}

	// Copy the next new bytes from ring[0..size) into out. The caller passes
	// the current producer write pointer (msgbuf.msg_bufx). A full ring is
	// indistinguishable from an empty one; returning zero there is deliberate
	// and only delays output until the producer advances.
	uint32_t take(const char *ring, uint32_t write, uint8_t *out, uint32_t outCapacity)
	{
		if (!primed || size == 0 || outCapacity == 0 || ring == nullptr) {
			return 0;
		}
		uint32_t available;
		if (write >= cursor) {
			available = write - cursor;
		} else {
			available = size - cursor + write;
		}
		if (available > outCapacity) {
			available = outCapacity;
		}
		if (available == 0) {
			return 0;
		}
		uint32_t first = size - cursor;
		if (first > available) {
			first = available;
		}
		memcpy(out, ring + cursor, first);
		if (available > first) {
			memcpy(out + first, ring, available - first);
		}
		cursor = (cursor + available) % size;
		return available;
	}
};

#endif /* UKE_USB_LOG_H */
