// SPDX-License-Identifier: BSD-2-Clause-Patent
// CDC-ACM descriptor construction for the uke debug serial. Pure C++ so the
// same bytes can be checked by a host test before the kernel build.
#ifndef UKE_USB_DESCRIPTORS_H
#define UKE_USB_DESCRIPTORS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define UKE_USB_DESC_VID 0x0525U
#define UKE_USB_DESC_PID 0xa4a7U

struct UkeUsbDescriptors {
	uint8_t device[18];
	uint8_t config[80];
	uint16_t configLength;
	uint8_t stringLang[4];
	uint8_t stringManufacturer[32];
	uint8_t stringProduct[40];
	uint8_t stringSerial[40];
	uint8_t stringLangLength;
	uint8_t stringManufacturerLength;
	uint8_t stringProductLength;
	uint8_t stringSerialLength;

	void build(uint16_t vid = UKE_USB_DESC_VID, uint16_t pid = UKE_USB_DESC_PID);
};

namespace UkeUsbDescriptorDetail {
inline void put8(uint8_t *&p, uint8_t v) { *p++ = v; }
inline void put16(uint8_t *&p, uint16_t v) { *p++ = (uint8_t)(v & 0xff); *p++ = (uint8_t)(v >> 8); }
inline uint8_t makeString(uint8_t *out, size_t cap, const char *ascii)
{
	size_t n = 0;
	while (ascii[n] != '\0' && (2 * n + 4) <= cap) {
		n++;
	}
	if (n == 0) {
		out[0] = 2;
		out[1] = 3;
		return 2;
	}
	size_t length = 2 + 2 * n;
	out[0] = (uint8_t)length;
	out[1] = 3;
	for (size_t i = 0; i < n; i++) {
		out[2 + 2 * i] = (uint8_t)ascii[i];
		out[3 + 2 * i] = 0;
	}
	return (uint8_t)length;
}
}

inline void UkeUsbDescriptors::build(uint16_t vid, uint16_t pid)
{
	using namespace UkeUsbDescriptorDetail;
	memset(this, 0, sizeof(*this));

	uint8_t *d = device;
	put8(d, 18);            // bLength
	put8(d, 1);             // DEVICE
	put16(d, 0x0200);       // bcdUSB 2.00
	put8(d, 0x02);          // bDeviceClass = CDC
	put8(d, 0x00);          // bDeviceSubClass
	put8(d, 0x00);          // bDeviceProtocol
	put8(d, 64);            // bMaxPacketSize0
	put16(d, vid);
	put16(d, pid);
	put16(d, 0x0100);       // bcdDevice
	put8(d, 1);             // iManufacturer
	put8(d, 2);             // iProduct
	put8(d, 3);             // iSerialNumber
	put8(d, 1);             // bNumConfigurations

	uint8_t *p = config;
	// Configuration descriptor; wTotalLength is filled after the body.
	put8(p, 9);
	put8(p, 2);
	uint8_t *total = p;
	put16(p, 0);
	put8(p, 2);             // bNumInterfaces
	put8(p, 1);             // bConfigurationValue
	put8(p, 0);             // iConfiguration
	put8(p, 0x80);          // bmAttributes: bus powered
	put8(p, 50);            // bMaxPower: 100 mA

	// Interface association: CDC control + data.
	put8(p, 8);
	put8(p, 0x0b);          // IAD
	put8(p, 0);
	put8(p, 2);
	put8(p, 0x02);
	put8(p, 0x02);
	put8(p, 0x01);
	put8(p, 0);

	// CDC control interface.
	put8(p, 9);
	put8(p, 4);
	put8(p, 0);
	put8(p, 0);
	put8(p, 1);             // one notification endpoint
	put8(p, 0x02);
	put8(p, 0x02);
	put8(p, 0x01);
	put8(p, 0);

	// CDC functional descriptors.
	put8(p, 5);  put8(p, 0x24); put8(p, 0x00); put8(p, 0x10); put8(p, 0x01);
	put8(p, 5);  put8(p, 0x24); put8(p, 0x01); put8(p, 0x00); put8(p, 0x01);
	put8(p, 4);  put8(p, 0x24); put8(p, 0x02); put8(p, 0x02);
	put8(p, 5);  put8(p, 0x24); put8(p, 0x06); put8(p, 0x00); put8(p, 0x01);

	// Notification endpoint: interrupt IN, 16 bytes, 16 ms.
	put8(p, 7); put8(p, 5); put8(p, 0x81); put8(p, 0x03); put16(p, 16); put8(p, 16);

	// CDC data interface.
	put8(p, 9);
	put8(p, 4);
	put8(p, 1);
	put8(p, 0);
	put8(p, 2);             // two bulk endpoints
	put8(p, 0x0a);
	put8(p, 0x00);
	put8(p, 0x00);
	put8(p, 0);

	// Bulk IN and bulk OUT, high-speed 512-byte packets.
	put8(p, 7); put8(p, 5); put8(p, 0x82); put8(p, 0x02); put16(p, 512); put8(p, 0);
	put8(p, 7); put8(p, 5); put8(p, 0x03); put8(p, 0x02); put16(p, 512); put8(p, 0);

	configLength = (uint16_t)(p - config);
	total[0] = (uint8_t)(configLength & 0xff);
	total[1] = (uint8_t)(configLength >> 8);

	stringLang[0] = 4; stringLang[1] = 3; stringLang[2] = 0x09; stringLang[3] = 0x04;
	stringLangLength = 4;
	stringManufacturerLength = makeString(stringManufacturer, sizeof(stringManufacturer), "UniXNU");
	stringProductLength = makeString(stringProduct, sizeof(stringProduct), "UKE USB Serial");
	stringSerialLength = makeString(stringSerial, sizeof(stringSerial), "UKE0001");
}

#endif /* UKE_USB_DESCRIPTORS_H */
