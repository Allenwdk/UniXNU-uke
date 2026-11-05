/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Clients of the WPSS (WifiProbe.cpp): a client registers a GLINK channel by name at any time (before or after the WPSS is up) and
 * gets its state and messages through callbacks that run on the WPSS thread; they must not block (copy what they need and wake their own
 * thread). Sends are queued and go out from the WPSS thread, so a caller never waits for the WPSS. */
#ifndef UKE_WPSS_H
#define UKE_WPSS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define UKE_WPSS_MAX_MESSAGE 0x4400u

typedef struct UkeWpssChannel UkeWpssChannel;
typedef struct {
    void (*state)(void *context, int up);
    void (*receive)(void *context, const uint8_t *data, uint32_t length);
} UkeWpssClient;

/* Registers the channel; intent_size/intent_count are the receive buffers offered to the WPSS. initiator: we open it (otherwise we answer
 * the WPSS's open). Returns null when out of slots or memory. Weak-imported by clients: the WPSS code may be absent from a build. */
UkeWpssChannel *uke_wpss_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                     const UkeWpssClient *client, void *context);
/* Queues one message (at most UKE_WPSS_MAX_MESSAGE bytes): 0 queued, -1 queue full or too big, -2 bad arguments. */
int uke_wpss_send(UkeWpssChannel *channel, const void *data, uint32_t length);

#ifdef __cplusplus
}
#endif
#endif
