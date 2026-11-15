/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Exercise the production timeout action against real protocol state. */
#include "ShengBattmgr.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {++checks; assert(x);} while (0)
constexpr unsigned kRequestMs=2000,kRetryMs=5000,kRing=16,kMessage=560;
static uint64_t nowMs;
static uint64_t mach_absolute_time() {return nowMs;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) {*ns=t*1000000;}
struct OSObject {};
struct IOTimerEventSource {unsigned ms=0;void setTimeoutMS(unsigned v) {ms=v;}void cancelTimeout() {ms=0;}};
struct Lock {};
static void IOSimpleLockLock(Lock *) {}
static void IOSimpleLockUnlock(Lock *) {}
#define OSDynamicCast(type, value) static_cast<type *>(value)
static void put(uint8_t *p,uint32_t v) {for (unsigned i=0;i<4;++i) p[i]=v>>(8*i);}
struct ShengBattery:OSObject {
    bool wasUp=true,published=true,unavailable=false;
    unsigned transportGeneration=1,subscriptions=0,publications=0,updates=0;
    unsigned ringHead=0,ringCount=0,messageLogs=8;
    Lock lock,*ringLock=&lock;
    struct {unsigned length;uint8_t data[kMessage];} ring[kRing];
    uint64_t requestDeadline=0;
    IOTimerEventSource rt,ft,*request=&rt,*refresh=&ft;
    ShengBattmgr bm;
    uint8_t sent[24]={};
    ShengBattery() {sheng_battmgr_init(&bm);bm.enabled=1;}
    void pump() {
        if (!bm.enabled || bm.poisoned) return;
        if (sheng_battmgr_next(&bm,sent)) {requestDeadline=nowMs+kRequestMs;request->setTimeoutMS(kRequestMs);}
    }
    void subscribe() {++subscriptions;bm.notification_pending=1;requestDeadline=nowMs+kRequestMs;request->setTimeoutMS(kRequestMs);}
    void stale(bool v) {unavailable=v;}
    void updateStatus() {++updates;}
    void handle(int r) {if (r==BmCycleDone) ++publications;pump();}
    // PRODUCTION_DRAIN_INBOX
    // PRODUCTION_REQUEST_ACTION
};
int main() {
    ShengBattery b;nowMs=100;sheng_battmgr_refresh(&b.bm,1);b.pump();
    CHECK(b.requestDeadline==2100 && b.bm.in_flight);
    auto *pending=b.ring[0].data;
    memcpy(pending,b.sent,12);put(pending+12,0);put(pending+16,1);put(pending+20,0);
    b.ring[0].length=24;b.ringCount=1;
    nowMs=2200;ShengBattery::requestAction(&b,nullptr);
    CHECK(!b.ringCount && b.bm.responses==1 && !b.bm.timeouts);
    CHECK(b.bm.cursor==BmPresent && b.bm.in_flight && b.requestDeadline==4200);
    CHECK(b.request->ms==2000 && !b.unavailable);
    nowMs=2700;ShengBattery::requestAction(&b,nullptr);
    CHECK(!b.bm.timeouts && b.request->ms==1500);
    nowMs=4200;ShengBattery::requestAction(&b,nullptr);
    CHECK(b.bm.timeouts==1 && !b.bm.poisoned && b.bm.last_timeout==BmPresent);
    CHECK(b.bm.cursor==BmCapacity && b.bm.in_flight && b.unavailable && b.updates==1);
    CHECK(b.refresh->ms==kRetryMs && b.requestDeadline==6200);
    // The last supported field timing out still delivers BmCycleDone.
    ShengBattery last;last.bm.cursor=BmChargeCurrent;last.pump();nowMs=last.requestDeadline;
    ShengBattery::requestAction(&last,nullptr);
    CHECK(last.publications==1 && last.bm.cycles_done==1 && !last.bm.poisoned);
    // Missing subscription acknowledgement retries, rather than disabling the service.
    ShengBattery sub;sub.bm.enabled=0;sub.subscribe();nowMs=sub.requestDeadline;
    ShengBattery::requestAction(&sub,nullptr);
    CHECK(sub.bm.timeouts==1 && !sub.bm.poisoned && !sub.bm.notification_pending);
    CHECK(sub.request->ms==kRetryMs);
    nowMs+=kRetryMs;ShengBattery::requestAction(&sub,nullptr);
    CHECK(sub.subscriptions==2 && sub.bm.notification_pending && sub.request->ms==kRequestMs);
    printf("PASS production battery timeout action, queued answer and rearmed deadline: %u checks\n",checks);
}
