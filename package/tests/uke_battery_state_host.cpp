/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The generated main embeds the production inbox channel-state block. */
#include "../ShengBattmgr.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, currentGeneration = 1;
#define CHECK(x) do {++checks; assert(x);} while (0)
static uint32_t sheng_adsp_generation() {return currentGeneration;}
constexpr unsigned kRefreshMs = 60000;
struct Timer {
    bool armed = true;
    void cancelTimeout() {armed = false;}
    void setTimeoutMS(unsigned) {armed = true;}
};
struct ShengBattery {
    unsigned channelChanged = 0, channelLost = 0, channelUp = 0;
    bool wasUp = false, published = false, unavailable = true;
    unsigned transportGeneration = 0, subscriptions = 0, updates = 0, messageLogs = 0;
    ShengBattmgr bm = {};
    Timer requestTimer, refreshTimer;
    Timer *request = &requestTimer, *refresh = &refreshTimer;
    void subscribe() {++subscriptions; bm.notification_pending = 1;}
    void stale(bool value) {unavailable = value;}
    void updateStatus() {++updates;}
};
void productionState(ShengBattery *self);
int main()
{
    ShengBattery b;
    b.channelChanged = b.channelUp = 1;
    productionState(&b);
    CHECK(b.wasUp && b.transportGeneration == 1 && b.subscriptions == 1);
    b.published = true;
    b.unavailable = false;
    b.bm.valid = 0xffff;
    b.bm.in_flight = 1;
    b.channelChanged = b.channelLost = 1;
    b.channelUp = 0;
    productionState(&b);
    CHECK(!b.wasUp && b.bm.poisoned && !b.bm.valid);
    CHECK(b.unavailable && b.updates == 1);
    CHECK(!b.request->armed && !b.refresh->armed);

    // A channel reopen does not prove an untagged request has drained.
    b.channelChanged = b.channelUp = 1;
    productionState(&b);
    CHECK(b.wasUp && b.bm.poisoned && b.subscriptions == 1);
    // Coalesced down/up must still invalidate the active snapshot.
    b.bm.poisoned = 0;
    b.bm.valid = 0xffff;
    b.unavailable = false;
    b.channelChanged = b.channelLost = b.channelUp = 1;
    productionState(&b);
    CHECK(b.bm.poisoned && !b.bm.valid && b.unavailable);
    CHECK(b.subscriptions == 1 && b.updates == 2);

    currentGeneration = 2;
    b.channelChanged = b.channelLost = b.channelUp = 1;
    productionState(&b);
    CHECK(!b.bm.poisoned && b.transportGeneration == 2);
    CHECK(b.subscriptions == 2 && b.bm.notification_pending && b.refresh->armed);
    CHECK(b.unavailable); // The next complete refresh controls publication.
    printf("PASS production battery channel state: %u checks\n", checks);
}
