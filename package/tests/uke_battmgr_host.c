/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../ShengBattmgr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static void put(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = v >> (8 * i);}
static uint32_t get(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

static void response(uint8_t *out, const uint8_t *in, uint32_t value)
{
    memcpy(out, in, 12);
    put(out + 12, get(in + 16));
    put(out + 16, value);
    put(out + 20, 0);
}

static int measurement(unsigned opcode, unsigned property)
{
    if (opcode == SHENG_BATTMGR_OP_USB_GET) return property <= 7;
    if (opcode != SHENG_BATTMGR_OP_BATTERY_GET) return 0;
    return property == 0 || property == 2 || property == 3 || property == 4 || property == 7 ||
        property == 9 || property == 10 || property == 12 || property == 13 ||
        property == 15 || property == 16 || property == 17 || property == 18 || property == 20 || property == 21;
}

int main(void)
{
    ShengBattmgr b;
    uint8_t request[28], reply[160], malformed[24];
    sheng_battmgr_init(&b);
    CHECK(sheng_battmgr_enable_request(request) == 28);
    CHECK(get(request) == 32778 && get(request + 4) == 2 && get(request + 8) == 4);
    b.notification_pending = 1;
    sheng_battmgr_refresh(&b, 1);
    CHECK(!sheng_battmgr_next(&b, request));
    memcpy(reply, request, 12);
    CHECK(sheng_battmgr_receive(&b, reply, 12) == BmEnabled);
    CHECK(b.enabled && !b.notification_pending);
    CHECK(sheng_battmgr_receive(&b, reply, 12) == BmIgnored);

    unsigned seen = 0;
    while (sheng_battmgr_next(&b, request)) {
        const unsigned opcode = get(request + 8), property = get(request + 16);
        CHECK(measurement(opcode, property));
        CHECK(!sheng_battmgr_next(&b, malformed));
        uint32_t value = 0;
        if (opcode == SHENG_BATTMGR_OP_BATTERY_GET) {
            if (property == 0 || property == 2) value = 1;
            if (property == 4) value = 6549;
            if (property == 7) value = 8100000;
            if (property == 9) value = (uint32_t)-1234567;
            if (property == 13) value = (uint32_t)-250;
            if (property == 20) value = 3660;
        } else if (property == 0) value = 1;
        response(reply, request, value);
        memcpy(malformed, reply, 24);
        put(malformed, 32779);
        CHECK(sheng_battmgr_receive(&b, malformed, 24) == BmIgnored);
        put(malformed, 32778);
        put(malformed + 4, 2);
        CHECK(sheng_battmgr_receive(&b, malformed, 24) == BmIgnored);
        CHECK(sheng_battmgr_receive(&b, reply, 23) == BmIgnored);
        CHECK(sheng_battmgr_receive(&b, reply, 25) == BmIgnored);
        const int rc = sheng_battmgr_receive(&b, reply, 24);
        CHECK(rc == BmAnswer || rc == BmCycleDone);
        CHECK(++seen <= BmFields);
    }
    CHECK(b.cycles_done == 1 && seen == 22);
    ShengBatterySnapshot snapshot;
    sheng_battmgr_snapshot(&b, &snapshot);
    CHECK(snapshot.percent == 65 && snapshot.voltage_mv == 8100);
    CHECK(snapshot.current_ma == 1234 && snapshot.temperature == -250);
    CHECK(snapshot.present && snapshot.external && snapshot.charging);
    CHECK(snapshot.time_to_full_min == 61);
    CHECK(!sheng_battmgr_next(&b, request));

    sheng_battmgr_refresh(&b, 0);
    CHECK(sheng_battmgr_next(&b, request));
    response(reply, request, 1);
    sheng_battmgr_timeout(&b);
    CHECK(b.poisoned && !b.valid);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmIgnored);
    sheng_battmgr_refresh(&b, 1);
    CHECK(!sheng_battmgr_next(&b, request));

    // Only a confirmed clean transport may initialize a fresh state.
    sheng_battmgr_init(&b);
    sheng_battmgr_refresh(&b, 1);
    CHECK(sheng_battmgr_next(&b, request));
    response(reply, request, 5);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmAnswer);
    CHECK(!(b.valid & (1u << BmStatus)) && b.errors == 1);

    sheng_battmgr_init(&b);
    b.notification_pending = 1;
    sheng_battmgr_timeout(&b);
    CHECK(b.poisoned && b.last_timeout == -2 && !b.valid);
    uint8_t notify[16] = {0};
    put(notify, 32778); put(notify + 4, 2); put(notify + 8, 7);
    CHECK(sheng_battmgr_receive(&b, notify, sizeof notify) == BmIgnored);
    printf("PASS Uke battery read-only and timeout quarantine: %u checks\n", checks);
}
