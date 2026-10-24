// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host-side checks for the pure USB serial building blocks. No XNU headers.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "UkeUsbDwc3.h"
#include "UkeUsbLog.h"
#include "UkeUsbPhy.h"
#include "UsbDescriptors.h"
#include "UsbRing.h"

static void testDescriptors()
{
	UkeUsbDescriptors d;
	d.build();
	assert(d.device[0] == 18);
	assert(d.device[1] == 1);
	assert(d.device[2] == 0x00 && d.device[3] == 0x02);
	assert(d.device[7] == 64);
	assert(d.device[8] == 0x25 && d.device[9] == 0x05);
	assert(d.device[10] == 0xa7 && d.device[11] == 0xa4);
	assert(d.device[17] == 1);
	assert(d.configLength == 75);
	assert(d.config[2] == 75 && d.config[3] == 0);
	assert(d.config[4] == 2);
	// IAD at 9, control interface at 17, data interface at 52.
	assert(d.config[9] == 8 && d.config[10] == 0x0b);
	assert(d.config[17] == 9 && d.config[18] == 4 && d.config[22] == 0x02 && d.config[23] == 0x02);
	assert(d.config[45] == 7 && d.config[46] == 5 && d.config[47] == 0x81 && d.config[48] == 0x03);
	assert(d.config[52] == 9 && d.config[53] == 4 && d.config[54] == 1 && d.config[57] == 0x0a);
	assert(d.config[61] == 7 && d.config[62] == 5 && d.config[63] == 0x82);
	assert(d.config[64] == 0x02 && d.config[65] == 0x00 && d.config[66] == 0x02);
	assert(d.config[68] == 7 && d.config[69] == 5 && d.config[70] == 0x03);
	assert(d.stringLangLength == 4 && d.stringLang[2] == 0x09 && d.stringLang[3] == 0x04);
	assert(d.stringProductLength > 2 && d.stringProduct[1] == 3);
	puts("descriptors: PASS");
}

static void testTrbRing()
{
	UkeUsbTrb memory[4];
	UkeUsbTrbRing ring;
	ring.init(memory, 4);
	assert(ring.empty() && ring.space() == 4);
	for (uint32_t i = 0; i < 4; i++) {
		UkeUsbTrb *slot = ring.producerSlot();
		assert(slot != nullptr);
		slot->ctrl = i;
		ring.commitProducer();
	}
	assert(ring.full() && ring.producerSlot() == nullptr);
	for (uint32_t i = 0; i < 4; i++) {
		UkeUsbTrb *slot = ring.consumerSlot();
		assert(slot != nullptr && slot->ctrl == i);
		ring.commitConsumer();
	}
	assert(ring.empty());
	// Wrap around.
	for (uint32_t i = 0; i < 2; i++) {
		ring.producerSlot()->ctrl = 0x100 + i;
		ring.commitProducer();
	}
	assert(ring.consumerSlot()->ctrl == 0x100);
	ring.commitConsumer();
	puts("trb ring: PASS");
}

static void testByteRing()
{
	uint8_t memory[8];
	UkeUsbByteRing ring;
	ring.init(memory, 8);
	assert(ring.writeBytes("abcdef", 6, false) == 6);
	uint8_t out[8] = {};
	assert(ring.readBytes(out, 2) == 2 && memcmp(out, "ab", 2) == 0);
	assert(ring.writeBytes("ghij", 4, true) == 4);
	assert(ring.dropped == 0);
	assert(ring.available() == 8);
	// Force drops while the ring is full: ten bytes into an eight-byte ring.
	assert(ring.writeBytes("0123456789", 10, true) == 10);
	assert(ring.dropped == 10);
	memset(out, 0, sizeof(out));
	assert(ring.readBytes(out, 8) == 8);
	assert(memcmp(out, "23456789", 8) == 0);
	puts("byte ring: PASS");
}

static void testLogCursor()
{
	char ring[16];
	for (uint32_t i = 0; i < 16; i++) {
		ring[i] = (char)('A' + i);
	}
	UkeUsbLogCursor cursor = {};
	cursor.prime(16, 16, true);
	uint8_t out[32] = {};
	assert(cursor.take(ring, 16, out, sizeof(out)) == 16);
	assert(memcmp(out, "ABCDEFGHIJKLMNOP", 16) == 0);
	// Producer wrapped and wrote four more bytes.
	memcpy(ring, "QRST", 4);
	assert(cursor.take(ring, 4, out, sizeof(out)) == 4);
	assert(memcmp(out, "QRST", 4) == 0);
	// No new data.
	assert(cursor.take(ring, 4, out, sizeof(out)) == 0);
	// Partial-line / bounded copy.
	cursor.prime(16, 10, true);
	assert(cursor.take(ring, 10, out, 3) == 3);
	assert(cursor.take(ring, 10, out, 3) == 3);
	assert(cursor.take(ring, 10, out, 3) == 3);
	assert(cursor.take(ring, 10, out, 3) == 1);
	assert(cursor.take(ring, 10, out, 3) == 0);
	puts("log cursor: PASS");
}

static void testPhyTable()
{
	assert(UKE_USB_PHY_INIT_COUNT > 10);
	bool sawOverride = false;
	bool sawPll = false;
	bool sawDiv = false;
	for (size_t i = 0; i < UKE_USB_PHY_INIT_COUNT; i++) {
		const UkeUsbPhyWrite &w = kUkeUsbPhyInit38_4Mhz[i];
		assert(w.offset < UKE_USB_PHY_SIZE);
		assert(w.mask != 0);
		if (w.offset == UKE_USB_PHY_CFG_CTRL_1 && w.value == 0 && w.mask == 0xff) {
			sawOverride = true;
		}
		if (w.offset == UKE_USB_PHY_CFG_CTRL_2 && w.value == 0xc8 && w.mask == 0xff) {
			sawPll = true;
		}
		if (w.offset == UKE_USB_PHY_CFG_CTRL_3 && w.value == 0x00 && w.mask == 0xff) {
			sawDiv = true;
		}
	}
	assert(sawOverride && sawPll && sawDiv);
	puts("phy table: PASS");
}

int main()
{
	testDescriptors();
	testTrbRing();
	testByteRing();
	testLogCursor();
	testPhyTable();
	puts("ALL PASS");
	return 0;
}
