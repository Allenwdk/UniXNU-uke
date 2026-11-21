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

static void check_pen_address_after_detach(void)
{
    ShengBattmgr b;
    uint8_t request[24], reply[24], late[24], mac[6];
    sheng_battmgr_init(&b);
    b.pen_diagnostics = 1;
    const uint64_t low = UINT64_C(1) << BmPenMacLow;
    const uint64_t tx = UINT64_C(1) << BmPenTxState;
    // Timeout while docked, stay docked, an invalid Hall reading, a
    // confirmed detach, and then a new dock on the same transport.
    const uint32_t hall[] = {0, 0, 2, 1, 0};
    for (unsigned round = 0; round < sizeof hall / sizeof *hall; ++round) {
        sheng_battmgr_refresh(&b, 0);
        int requested_low = 0, requested_high = 0;
        while (sheng_battmgr_next(&b, request)) {
            const unsigned property = get(request + 16);
            uint32_t value = 1;
            if (get(request + 8) == SHENG_BATTMGR_OP_BOARD_GET) {
                if (property == 0x64) value = hall[round];
                if (property == 0x6a && round == 0) {
                    CHECK(sheng_battmgr_timeout(&b) == BmAnswer);
                    continue;
                }
                if (property == 0x5f) {
                    requested_low = 1;
                    value = 0xa3a4a5a6;
                    if (round == 0) {
                        response(late, request, value);
                        CHECK(sheng_battmgr_timeout(&b) == BmAnswer);
                        continue;
                    }
                }
                if (property == 0x60) {
                    requested_high = 1;
                    value = 0xa1a2;
                    CHECK(sheng_battmgr_receive(&b, late, 24) == BmIgnored);
                }
            }
            response(reply, request, value);
            const int rc = sheng_battmgr_receive(&b, reply, 24);
            CHECK(rc == BmAnswer || rc == BmCycleDone);
        }
        CHECK(requested_low == (round == 0 || round == 4));
        CHECK(requested_high == (hall[round] == 0));
        CHECK(b.pen_unavailable == (round < 3 ? low | tx : tx));
        CHECK(sheng_battmgr_pen_address(&b, mac) == (round == 4));
    }
    CHECK(memcmp(mac, (uint8_t[]){0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6}, sizeof mac) == 0);
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
    CHECK(sheng_battmgr_timeout(&b) == BmAnswer);
    CHECK(!b.poisoned && !(b.valid & (1u << BmStatus)));
    CHECK(sheng_battmgr_next(&b, request) == 24);
    CHECK(get(request + 16) == 2); // Continue with present, not the expired status.
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmIgnored);
    sheng_battmgr_refresh(&b, 1);
    CHECK(!sheng_battmgr_next(&b, request));

    // A confirmed clean transport may also initialize a fresh state.
    sheng_battmgr_init(&b);
    sheng_battmgr_refresh(&b, 1);
    CHECK(sheng_battmgr_next(&b, request));
    response(reply, request, 5);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmAnswer);
    CHECK(!(b.valid & (1u << BmStatus)) && b.errors == 1);

    sheng_battmgr_init(&b);
    b.cursor = BmTemperature;
    CHECK(sheng_battmgr_next(&b, request));
    response(reply, request, 0x7fffffff);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmAnswer);
    CHECK(!(b.valid & (1u << BmTemperature)) && b.errors == 1);
    sheng_battmgr_init(&b);
    b.cursor = BmCurrent;
    CHECK(sheng_battmgr_next(&b, request));
    response(reply, request, 0x80000000u);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmAnswer);
    CHECK(!(b.valid & (1u << BmCurrent)) && b.errors == 1);
    b.value[BmUsbOnline] = 1;
    b.value[BmStatus] = 1;
    b.value[BmCapacity] = 10000;
    b.valid = (1u << BmUsbOnline) | (1u << BmStatus) | (1u << BmCapacity);
    sheng_battmgr_snapshot(&b, &snapshot);
    CHECK(snapshot.charging && !snapshot.fully_charged);
    b.value[BmStatus] = 4;
    sheng_battmgr_snapshot(&b, &snapshot);
    CHECK(!snapshot.charging && snapshot.fully_charged);

    sheng_battmgr_init(&b);
    b.notification_pending = 1;
    CHECK(sheng_battmgr_timeout(&b) == BmAnswer);
    CHECK(!b.poisoned && b.last_timeout == -2 && !b.notification_pending);
    uint8_t notify[16] = {0};
    put(notify, 32778); put(notify + 4, 2); put(notify + 8, 7);
    CHECK(sheng_battmgr_receive(&b, notify, sizeof notify) == BmNotified);

    sheng_battmgr_init(&b);
    b.pen_diagnostics = 1;
    static const uint32_t pen_properties[] = {0x64, 0x65, 0x48, 0x63, 0x6a, 0x61, 0x62, 0x6b};
    for (unsigned round = 0; round < 2; ++round) {
        sheng_battmgr_refresh(&b, 0);
        unsigned i = round;
        while (sheng_battmgr_next(&b, request)) {
            const unsigned opcode = get(request + 8), property = get(request + 16);
            if (opcode != SHENG_BATTMGR_OP_BOARD_GET) {
                CHECK(measurement(opcode, property));
                response(reply, request, property == 4 ? 5000 : 1);
            } else {
                CHECK(i < sizeof pen_properties / sizeof *pen_properties && property == pen_properties[i]);
                ++i;
                response(reply, request, property == 0x63 ? 255 : 1);
                if (property == 0x64) {
                    CHECK(sheng_battmgr_timeout(&b) == BmAnswer);
                    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmIgnored);
                    continue;
                }
            }
            const int rc = sheng_battmgr_receive(&b, reply, 24);
            CHECK(rc == BmAnswer || rc == BmCycleDone);
        }
        CHECK(i == sizeof pen_properties / sizeof *pen_properties);
        CHECK((b.valid >> BmPenHall3) == 0xf6 && (b.pen_unavailable >> BmPenHall3) == 1);
        CHECK(b.cycles_done == round + 1 && b.timeouts == 1);
        sheng_battmgr_snapshot(&b, &snapshot);
        CHECK(snapshot.charge_known && snapshot.percent == 50 && snapshot.charging);
    }
    sheng_battmgr_init(&b);
    b.pen_diagnostics = 1;
    b.cursor = BmPenMacLow;
    b.valid = UINT64_C(1) << BmPenHall3;
    CHECK(sheng_battmgr_next(&b, request) == 24);
    CHECK(get(request + 16) == 0x5f);
    response(reply, request, 0xa3a4a5a6);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmAnswer);
    CHECK(sheng_battmgr_next(&b, request) == 24);
    CHECK(get(request + 16) == 0x60);
    response(reply, request, 0xa1a2);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmCycleDone);
    uint8_t mac[6] = {0};
    CHECK(sheng_battmgr_pen_address(&b, mac));
    CHECK(memcmp(mac, (uint8_t[]){0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6}, 6) == 0);
    b.value[BmPenHall3] = 1;
    CHECK(!sheng_battmgr_pen_address(&b, mac));

    check_pen_address_after_detach();
    printf("PASS Uke battery read-only, per-request timeout and dock address: %u checks\n", checks);
}
