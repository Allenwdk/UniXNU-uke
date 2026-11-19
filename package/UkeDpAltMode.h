// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef UKE_DP_ALT_MODE_H
#define UKE_DP_ALT_MODE_H

#include "QcomUcsi.h"

// PMIC GLINK PAN protocol. The Uke OEM service and Linux's SC8280XP
// notification format both use owner 32780 on the PMIC transport.
namespace UkeDpAltMode {
constexpr uint32_t kOwner = 32780, kHeaderBytes = 12;
constexpr uint32_t kRequestResponse = 1, kNotification = 2, kWrite = 0x15, kNotify = 0x16;
constexpr unsigned kRequestBytes = 24, kNotificationBytes = 32;
constexpr uint16_t kDisplayPortSvid = 0xff01;

enum class Command : uint32_t { None, EnableNotifications = 0x10, AckNotification = 0x11 };

struct Notification {
    uint16_t svid;
    uint8_t orientation, mux, pin, lanes;
    bool hpd, irq, validRoute;
};

inline bool isOwner(const uint8_t *data, uint32_t length) {
    return length >= sizeof(uint32_t) && ucsi_get32(data) == kOwner;
}

inline bool isWriteResponse(const uint8_t *data, uint32_t length) {
    return length >= kHeaderBytes && isOwner(data, length) &&
           ucsi_get32(data + 4) == kRequestResponse && ucsi_get32(data + 8) == kWrite;
}

inline void writeRequest(uint8_t (&out)[kRequestBytes], Command command) {
    ucsi_put32(out, kOwner);
    ucsi_put32(out + 4, kRequestResponse);
    ucsi_put32(out + 8, kWrite);
    ucsi_put32(out + 12, static_cast<uint32_t>(command));
    ucsi_put32(out + 16, 0);  // Uke's only connector is PAN port 0.
    ucsi_put32(out + 20, 0);
}

inline bool decodeNotification(const uint8_t *data, uint32_t length, Notification &event) {
    if (length != kNotificationBytes || !isOwner(data, length) || data[12] != 0) return false;
    const uint32_t type = ucsi_get32(data + 4), opcode = ucsi_get32(data + 8);
    if ((type != kRequestResponse && type != kNotification) || (opcode & 0xffff) != kNotify) return false;
    event = {};
    event.svid = static_cast<uint16_t>(opcode >> 16);
    event.orientation = data[13]; // normal, reverse, disconnected
    event.mux = data[14];         // disconnected, USB, DP, USB + DP, tunnelling
    if (event.svid == kDisplayPortSvid) {
        const uint8_t dp = data[20];
        event.pin = dp & 0x3f;
        event.hpd = dp & 0x40;
        event.irq = dp & 0x80;
    }
    if (event.orientation < 2 && event.svid == kDisplayPortSvid && (!event.irq || event.hpd)) {
        if (event.mux == 2 && (event.pin == 3 || event.pin == 5)) event.lanes = 4; // C/E
        if (event.mux == 3 && (event.pin == 4 || event.pin == 6)) event.lanes = 2; // D/F
    }
    event.validRoute = event.lanes || (event.mux == 0 && event.orientation <= 2) ||
                       (event.mux == 1 && event.orientation < 2 && event.svid != kDisplayPortSvid);
    return true;
}
}
#endif
