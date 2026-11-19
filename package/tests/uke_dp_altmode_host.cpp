// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../UkeDpAltMode.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using namespace UkeDpAltMode;
    uint8_t request[kRequestBytes];
    const uint8_t enable[] = {
        0x0c, 0x80, 0, 0, 1, 0, 0, 0, 0x15, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    writeRequest(request, Command::EnableNotifications);
    assert(std::memcmp(request, enable, sizeof request) == 0);
    writeRequest(request, Command::AckNotification);
    assert(request[12] == 0x11 && request[16] == 0 && request[20] == 0);

    uint8_t packet[kNotificationBytes] = {
        0x0c, 0x80, 0, 0, 2, 0, 0, 0, 0x16, 0, 1, 0xff, 0, 1, 3, 0, 0, 0, 1, 0xff, 0xc4,
    };
    Notification event;
    assert(decodeNotification(packet, sizeof packet, event));
    assert(event.svid == kDisplayPortSvid && event.orientation == 1 && event.mux == 3);
    assert(event.pin == 4 && event.lanes == 2 && event.hpd && event.irq && event.validRoute);
    assert(!isWriteResponse(packet, sizeof packet));
    assert(!decodeNotification(packet, sizeof packet - 1, event));

    packet[4] = 1;
    packet[13] = 0;
    packet[14] = 2;
    packet[20] = 0x45;
    assert(decodeNotification(packet, sizeof packet, event) && event.validRoute && event.lanes == 4);
    packet[20] = 0x84;
    assert(decodeNotification(packet, sizeof packet, event) && !event.validRoute && event.lanes == 0);
    packet[13] = 2;
    packet[14] = packet[20] = 0;
    assert(decodeNotification(packet, sizeof packet, event) && event.validRoute && !event.hpd);

    packet[12] = 1;
    assert(!decodeNotification(packet, sizeof packet, event));
    packet[12] = 0;
    packet[4] = 3;
    assert(!decodeNotification(packet, sizeof packet, event));

    uint8_t response[kHeaderBytes] = {0x0c, 0x80, 0, 0, 1, 0, 0, 0, 0x15, 0, 0, 0};
    assert(isWriteResponse(response, sizeof response));
    assert(!isWriteResponse(response, sizeof response - 1));
    response[4] = 2;
    assert(!isWriteResponse(response, sizeof response));
    response[4] = 1;
    response[10] = 1;
    assert(!isWriteResponse(response, sizeof response));
    response[10] = 0;
    response[0] = 0x0b; // UCSI
    assert(!isWriteResponse(response, sizeof response));
    std::puts("Uke PAN wire format and DP notifications verified");
}
