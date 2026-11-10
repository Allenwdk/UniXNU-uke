/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../ShengBattmgr.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(value) do {++checks; assert(value);} while (0)
static void put(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = value >> (8 * i);
}
static uint32_t get(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t sample(uint32_t opcode, uint32_t property)
{
    static const uint32_t battery[22] = {
        [0] = 1, [2] = 1, [4] = 6549, [7] = 8100000,
        [9] = (uint32_t)-1234567, [13] = 2500, [18] = 10000000,
    };
    if (opcode == SHENG_BATTMGR_OP_USB_GET) return property == 0 ? 1 : 0;
    assert(opcode == SHENG_BATTMGR_OP_BATTERY_GET && property < 22);
    return battery[property];
}
static void notify(ShengBattmgr *b)
{
    uint8_t packet[16] = {0};
    put(packet, SHENG_BATTMGR_OWNER);
    put(packet + 4, SHENG_BATTMGR_TYPE_NOTIFY);
    put(packet + 8, SHENG_BATTMGR_OP_NOTIFICATION);
    CHECK(sheng_battmgr_receive(b, packet, sizeof packet) == BmNotified);
}

static unsigned cycle(ShengBattmgr *b, unsigned notifications)
{
    uint8_t request[24], response[24];
    unsigned reads = 0;
    for (;;) {
        CHECK(sheng_battmgr_next(b, request) == sizeof request);
        const uint32_t opcode = get(request + 8), property = get(request + 16);
        memcpy(response, request, 12);
        put(response + 12, property);
        put(response + 16, sample(opcode, property));
        put(response + 20, 0);
        if (notifications) notify(b);
        const int result = sheng_battmgr_receive(b, response, sizeof response);
        ++reads;
        CHECK(reads <= BmFields);
        if (result == BmCycleDone) return reads;
        CHECK(result == BmAnswer);
    }
}

int main(void)
{
    const uint32_t required = (1u << BmStatus) | (1u << BmPresent) | (1u << BmCapacity) |
        (1u << BmVoltage) | (1u << BmCurrent) | (1u << BmTemperature) | (1u << BmUsbOnline);
    ShengBattmgr b;
    ShengBatterySnapshot snapshot;
    uint8_t request[24];
    sheng_battmgr_init(&b);
    sheng_battmgr_refresh(&b, 1);
    CHECK(cycle(&b, 1) == 22);
    CHECK(b.cycles_done == 1 && (b.valid & required) == required);
    CHECK(b.refresh_wanted == 1 && !b.in_flight);
    sheng_battmgr_snapshot(&b, &snapshot);
    CHECK(snapshot.charge_known && snapshot.percent == 65 && snapshot.current_ma == 1234);
    CHECK(snapshot.present && snapshot.external && snapshot.charging);
    // Even a storm of notifications preserves the just-completed snapshot.
    CHECK(cycle(&b, 1) == 19);
    CHECK(b.cycles_done == 2 && (b.valid & required) == required);
    sheng_battmgr_snapshot(&b, &snapshot);
    CHECK(snapshot.percent == 65 && snapshot.voltage_mv == 8100);

    // A slow refresh cannot be replaced by a later fast notification.
    CHECK(sheng_battmgr_next(&b, request) == sizeof request);
    CHECK(!(b.valid & required));
    sheng_battmgr_refresh(&b, 1);
    notify(&b);
    CHECK(b.refresh_wanted == 2);
    // Transport timeout still invalidates everything and forbids that queued work.
    sheng_battmgr_timeout(&b);
    CHECK(b.poisoned && !b.valid);
    CHECK(!sheng_battmgr_next(&b, request));
    printf("PASS completed battery snapshot before queued refresh: %u checks\n", checks);
}
