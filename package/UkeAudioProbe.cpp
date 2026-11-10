/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Stage-one AudioReach readiness. Transport and GPR state machine are upstream.
 * No PCM endpoint is published until its own hardware route is qualified. */
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include "ShengAdsp.h"
#include "ShengGpr.h"

extern "C" {volatile int32_t native_audio_probe_stats[8];}

namespace {
constexpr unsigned kSlots = 8;
struct Reply {uint32_t bytes; uint8_t data[GPR_MAX_PACKET];};
Reply gReplies[kSlots];
IOSimpleLock *gLock;
ShengAdspChannel *gChannel;
unsigned gHead, gCount;
volatile unsigned gChanged, gUp;
GprSpfProbe gProbe;

void state(void *, int up)
{
    IOSimpleLockLock(gLock);
    gHead = gCount = 0;
    IOSimpleLockUnlock(gLock);
    __atomic_store_n(&gUp, up ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gChanged, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gChanged);
}

void receive(void *, const uint8_t *data, uint32_t bytes)
{
    IOSimpleLockLock(gLock);
    if (bytes <= GPR_MAX_PACKET && gCount < kSlots) {
        Reply &slot = gReplies[(gHead + gCount) % kSlots];
        memcpy(slot.data, data, bytes);
        slot.bytes = bytes;
        ++gCount;
    } else {
        native_audio_probe_stats[7] = native_audio_probe_stats[7] + 1;
    }
    IOSimpleLockUnlock(gLock);
    thread_wakeup((event_t)&gChanged);
}

void worker(void *, wait_result_t)
{
    bool up = false;
    uint64_t deadline = 0;
    gpr_spf_init(&gProbe, 1, 100);
    for (;;) {
        const bool currentUp = __atomic_load_n(&gUp, __ATOMIC_ACQUIRE) != 0;
        if (__atomic_exchange_n(&gChanged, 0u, __ATOMIC_ACQ_REL) || currentUp != up) {
            up = currentUp;
            const uint32_t token = gProbe.token;
            gpr_spf_init(&gProbe, 1, 100);
            gProbe.token = token; // Old responses cannot complete a query after a channel reopen.
            deadline = 0;
            native_audio_probe_stats[0] = up ? 2 : 1;
        }
        Reply slot = {};
        IOSimpleLockLock(gLock);
        if (gCount) {
            slot = gReplies[gHead];
            gHead = (gHead + 1) % kSlots;
            --gCount;
        }
        IOSimpleLockUnlock(gLock);
        if (slot.bytes && up) {
            if (gProbe.answers < 8) {
                char hex[65] = {};
                const unsigned bytes = slot.bytes < 32 ? slot.bytes : 32;
                for (unsigned i = 0; i < bytes; ++i) snprintf(hex + i * 2, 3, "%02x", slot.data[i]);
                printf("UKE AUDIO: channel=adsp_apps bytes=%u head=%s\n", slot.bytes, hex);
            }
            const int result = gpr_spf_receive(&gProbe, slot.data, slot.bytes);
            native_audio_probe_stats[3] = gProbe.answers;
            native_audio_probe_stats[4] = gProbe.last_status;
            if (result == SpfIsReady) {
                native_audio_probe_stats[0] = 3;
                printf("UKE AUDIO: APM ready token=%u queries=%u responses=%u\n",
                       gProbe.token, gProbe.queries, gProbe.answers);
            }
        }
        if (up && gProbe.state == SpfWaiting && mach_absolute_time() >= deadline) {
            gpr_spf_timeout(&gProbe);
            native_audio_probe_stats[5] = gProbe.timeouts;
        }
        if (up && gProbe.state == SpfIdle) {
            uint8_t packet[GPR_HEADER_BYTES];
            const uint32_t bytes = gpr_spf_next(&gProbe, packet);
            if (bytes && sheng_adsp_send(gChannel, packet, bytes)) {
                gProbe.state = SpfIdle;
                --gProbe.queries;
                native_audio_probe_stats[6] = native_audio_probe_stats[6] + 1;
            } else if (bytes) {
                native_audio_probe_stats[2] = gProbe.queries;
                clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
            }
        }
        if (gProbe.state == SpfGaveUp && native_audio_probe_stats[0] >= 0) {
            native_audio_probe_stats[0] = -1;
            printf("UKE AUDIO: APM query exhausted errors=%u timeouts=%u status=%u\n",
                   gProbe.errors, gProbe.timeouts, gProbe.last_status);
        }
        const unsigned ms = up && (gProbe.state == SpfIdle || gProbe.state == SpfWaiting) ? 100 : 3600u * 1000u;
        assert_wait_timeout((event_t)&gChanged, THREAD_UNINT, ms, kMillisecondScale);
        IOSimpleLockLock(gLock);
        const bool pending = gCount || __atomic_load_n(&gChanged, __ATOMIC_ACQUIRE);
        IOSimpleLockUnlock(gLock);
        if (pending) clear_wait(current_thread(), THREAD_AWAKENED);
        else thread_block(THREAD_CONTINUE_NULL);
    }
}
}

extern "C" void uke_audio_probe(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("audio", &enabled, sizeof enabled) || enabled != 1) return;
    gLock = IOSimpleLockAlloc();
    if (!gLock) return;
    const ShengAdspClient client = {state, receive};
    gChannel = sheng_adsp_channel("adsp_apps", GPR_MAX_PACKET, 20, 1, &client, nullptr);
    if (!gChannel) {IOSimpleLockFree(gLock); gLock = nullptr; return;}
    thread_t thread;
    native_audio_probe_stats[0] = 1;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else {
        native_audio_probe_stats[0] = -2;
        printf("UKE AUDIO: readiness worker allocation failed\n");
    }
}
