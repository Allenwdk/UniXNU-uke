/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* WPSS IPCRTR service discovery using sheng's original GLINK/QRTR cores.
 * This stage observes WLFW (service 0x45, WCN6750 instance 3). The ath11k
 * backend will own QMI requests; discovery does not impersonate a radio. */
#include <stdint.h>
#include <stddef.h>
#include <sys/systm.h>
#include "QcomQrtr.h"
#include "UkeWpss.h"

static QcomQrtr router;
static UkeWpssChannel *channel;
volatile uint32_t native_wifi_qrtr_stats[8];
/* stage, rx, bad, service-count, WLFW-node, WLFW-port, instance, links */

static int send(void *context, const uint8_t *packet, uint32_t length)
{
    (void)context;
    return uke_wpss_send(channel, packet, length);
}

static void state(void *context, int up)
{
    (void)context;
    native_wifi_qrtr_stats[0] = up ? 2 : 1;
    router.hello_sent = router.hello_received = 0;
    router.remote_count = 0;
    router.remote_node = QCOM_QRTR_NODE_BROADCAST;
    native_wifi_qrtr_stats[3] = 0;
    native_wifi_qrtr_stats[4] = native_wifi_qrtr_stats[5] = native_wifi_qrtr_stats[6] = 0;
    if (up) {
        native_wifi_qrtr_stats[7]++;
        qcom_qrtr_start(&router);
    }
    printf("UKE WIFI: IPCRTR link=%d\n", up);
}

static void receive(void *context, const uint8_t *data, uint32_t length)
{
    (void)context;
    qcom_qrtr_receive(&router, data, length);
    native_wifi_qrtr_stats[1] = router.rx_packets;
    native_wifi_qrtr_stats[2] = router.rx_bad;
    native_wifi_qrtr_stats[3] = router.remote_count;
    for (uint32_t i = 0; i < router.remote_count; ++i) {
        if (router.remote[i].service != 0x45 || (router.remote[i].instance >> 8) != 3) continue;
        if (native_wifi_qrtr_stats[0] == 3) return;
        native_wifi_qrtr_stats[4] = router.remote[i].node;
        native_wifi_qrtr_stats[5] = router.remote[i].port;
        native_wifi_qrtr_stats[6] = router.remote[i].instance;
        native_wifi_qrtr_stats[0] = 3;
        printf("UKE WIFI: WLFW service=45 node=%u port=%u instance=%x\n",
               router.remote[i].node, router.remote[i].port, router.remote[i].instance);
        return;
    }
    if (native_wifi_qrtr_stats[0] == 3) {
        native_wifi_qrtr_stats[0] = 2;
        native_wifi_qrtr_stats[4] = native_wifi_qrtr_stats[5] = native_wifi_qrtr_stats[6] = 0;
        printf("UKE WIFI: WLFW service removed\n");
    }
}

void uke_wifi_qrtr_start(void)
{
    const UkeWpssClient client = {state, receive};
    qcom_qrtr_init(&router, 1, send, NULL);
    /* The stock GLINK edge offers 0x800/0x2000/0x4400 receive buffers.
     * Uniform largest-size intents also admit all three message sizes. */
    channel = uke_wpss_channel("IPCRTR", 0x4400, 10, 0, &client, NULL);
    native_wifi_qrtr_stats[0] = channel ? 1 : 0;
}
