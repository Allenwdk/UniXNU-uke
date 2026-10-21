// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host-testable TRB and byte ring helpers. The kernel driver owns the DWC3
// cycle/link policy; these helpers only manage indices and bounds.
#ifndef UKE_USB_RING_H
#define UKE_USB_RING_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "UkeUsbDwc3.h"

struct UkeUsbTrbRing {
	UkeUsbTrb *ring;
	uint32_t capacity;
	uint32_t head;
	uint32_t tail;
	uint32_t used;

	void init(UkeUsbTrb *memory, uint32_t entries)
	{
		ring = memory;
		capacity = entries;
		head = tail = used = 0;
	}
	bool empty() const { return used == 0; }
	bool full() const { return used == capacity; }
	uint32_t count() const { return used; }
	uint32_t space() const { return capacity - used; }
	UkeUsbTrb *producerSlot()
	{
		return full() ? nullptr : &ring[head];
	}
	void commitProducer()
	{
		if (used < capacity) {
			head = (head + 1) % capacity;
			used++;
		}
	}
	UkeUsbTrb *consumerSlot()
	{
		return empty() ? nullptr : &ring[tail];
	}
	void commitConsumer()
	{
		if (used != 0) {
			tail = (tail + 1) % capacity;
			used--;
		}
	}
};

struct UkeUsbByteRing {
	uint8_t *data;
	uint32_t capacity;
	uint32_t read;
	uint32_t write;
	uint32_t used;
	uint32_t dropped;

	void init(uint8_t *memory, uint32_t bytes)
	{
		data = memory;
		capacity = bytes;
		read = write = used = dropped = 0;
	}
	uint32_t available() const { return used; }
	uint32_t space() const { return capacity - used; }
	uint32_t writeBytes(const void *source, uint32_t length, bool dropOldest)
	{
		const uint8_t *src = static_cast<const uint8_t *>(source);
		uint32_t accepted = 0;
		while (accepted < length) {
			if (used == capacity) {
				if (!dropOldest) {
					break;
				}
				read = (read + 1) % capacity;
				used--;
				dropped++;
			}
			data[write] = src[accepted++];
			write = (write + 1) % capacity;
			used++;
		}
		return accepted;
	}
	uint32_t readBytes(void *destination, uint32_t length)
	{
		uint8_t *dst = static_cast<uint8_t *>(destination);
		uint32_t copied = 0;
		while (copied < length && used != 0) {
			dst[copied++] = data[read];
			read = (read + 1) % capacity;
			used--;
		}
		return copied;
	}
};

#endif /* UKE_USB_RING_H */
