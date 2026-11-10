/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The checker inserts the production GLINK callbacks and gated callout. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static unsigned checks;
#define CHECK(value) do {++checks; assert(value);} while (0)
constexpr unsigned kRing = 16, kMessage = 560;
constexpr int kIOReturnSuccess = 0;
using IOReturn = int;
using thread_call_param_t = void *;
struct OSObject {};
struct Lock {unsigned held = 0;};
static void IOSimpleLockLock(Lock *lock) {assert(!lock->held); ++lock->held;}
static void IOSimpleLockUnlock(Lock *lock) {assert(lock->held == 1); --lock->held;}
struct Call {
    void (*function)(void *, void *);
    void *context;
    bool pending = false;
};
using thread_call_t = Call *;
static bool thread_call_enter(thread_call_t call) {
    const bool pending = call->pending;
    call->pending = true;
    return pending;
}
struct Loop {
    bool gated = false;
    unsigned actions = 0;
    IOReturn runAction(IOReturn (*action)(OSObject *, void *, void *, void *, void *), OSObject *owner) {
        CHECK(!gated);
        gated = true;
        ++actions;
        const auto result = action(owner, nullptr, nullptr, nullptr, nullptr);
        gated = false;
        return result;
    }
};
volatile uint32_t native_battery_dispatch[6];
volatile int32_t native_battery_stats[16];

struct ShengBattery : OSObject {
    Lock storage;
    Lock *ringLock = &storage;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    unsigned ringHead = 0, ringCount = 0, channelChanged = 0, channelLost = 0, channelUp = 0;
    Loop workLoop;
    Loop *loop = &workLoop;
    Call call;
    thread_call_t inboxCall = &call;
    std::vector<unsigned> packets;
    unsigned stateChanges = 0;
    bool injectDuringDrain = false;
    static void inboxAction(OSObject *owner);
    // PRODUCTION_CALLBACKS
};

void ShengBattery::inboxAction(OSObject *owner) {
    auto *self = static_cast<ShengBattery *>(owner);
    CHECK(self->loop->gated);
    if (self->channelChanged) {++self->stateChanges; self->channelChanged = 0;}
    while (self->ringCount) {
        const auto &slot = self->ring[self->ringHead];
        CHECK(slot.length && slot.length <= kMessage);
        self->packets.push_back(slot.data[0]);
        self->ringHead = (self->ringHead + 1) % kRing;
        --self->ringCount;
    }
    if (self->injectDuringDrain) {
        self->injectDuringDrain = false;
        const uint8_t value = 99;
        dspReceive(self, &value, 1);
    }
}

static void pump(ShengBattery &battery) {
    unsigned loops = 0;
    while (battery.call.pending) {
        CHECK(++loops < 10);
        battery.call.pending = false;
        battery.call.function(battery.call.context, nullptr);
    }
}

int main() {
    ShengBattery battery;
    battery.call.function = ShengBattery::deliverInbox;
    battery.call.context = &battery;
    ShengBattery::dspState(&battery, 1);
    CHECK(battery.channelUp && battery.channelChanged && battery.call.pending);
    CHECK(!battery.workLoop.actions && !battery.stateChanges);
    pump(battery);
    CHECK(battery.stateChanges == 1 && battery.workLoop.actions == 1);
    CHECK(native_battery_dispatch[0] == 1 && native_battery_dispatch[2] == 1 && native_battery_dispatch[3] == 1);
    CHECK(native_battery_dispatch[4] == 1 && native_battery_dispatch[5] == 1);

    uint8_t message[kMessage + 1] = {};
    for (unsigned i = 0; i < kRing; ++i) {
        message[0] = i;
        ShengBattery::dspReceive(&battery, message, kMessage);
    }
    CHECK(battery.ringCount == kRing && battery.workLoop.actions == 1);
    ShengBattery::dspReceive(&battery, message, 1);
    ShengBattery::dspReceive(&battery, message, kMessage + 1);
    CHECK(battery.ringCount == kRing && native_battery_stats[14] == 2);
    pump(battery);
    CHECK(battery.packets.size() == kRing && battery.workLoop.actions == 2);
    for (unsigned i = 0; i < kRing; ++i) CHECK(battery.packets[i] == i);

    // State changes discard prior channel messages and survive coalescing.
    ShengBattery::dspReceive(&battery, message, 1);
    ShengBattery::dspState(&battery, 0);
    ShengBattery::dspState(&battery, 1);
    CHECK(!battery.ringCount && battery.channelLost && battery.channelUp);
    pump(battery);
    CHECK(battery.stateChanges == 2 && battery.packets.size() == kRing);

    // An event arriving during the gated action schedules another pass.
    battery.injectDuringDrain = true;
    ShengBattery::dspReceive(&battery, message, 1);
    const unsigned actions = battery.workLoop.actions;
    pump(battery);
    CHECK(battery.workLoop.actions == actions + 2);
    CHECK(!battery.ringCount && battery.packets.back() == 99 && !battery.storage.held);
    CHECK(native_battery_dispatch[2] == native_battery_dispatch[3]);
    CHECK(native_battery_dispatch[4] == native_battery_dispatch[5]);
    printf("PASS production battery callbacks and gated callout: %u checks\n", checks);
}
