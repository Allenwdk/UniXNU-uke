/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Follow ath11k's fixed-memory WLFW server-arrival sequence through
 * device identity discovery. The full ath11k backend will own BDF and
 * the radio lifecycle. */
#include <stdint.h>
#include <stddef.h>
#include <sys/systm.h>
#include "QcomQrtr.h"
#include "UkeWpss.h"
#include "nw_wcn6750_qmi.h"

extern uint64_t uke_wpss_now_ms(void);

static QcomQrtr router;
static UkeWpssChannel *channel;
volatile uint32_t native_wifi_qrtr_stats[8];
/* stage, rx, bad, service-count, WLFW-node, WLFW-port, instance, links */
volatile uint32_t native_wifi_qmi_stats[10];
/* state, requests, responses, timeouts, unrelated, malformed, message,
 * transaction, QMI result, QMI error. States 6/7 wait for setup replies,
 * 1/2 wait for identity replies, 4/5 record CAP/BAR, negative is failure. */
struct nw_wcn6750_identity native_wifi_identity;
static uint32_t qmi_node, qmi_port;
static uint16_t qmi_message, qmi_transaction;
static uint64_t qmi_deadline;
static int qmi_pending;
enum {
    kClientPort = 0x4000,
    kRegister = 0x20, kHostCapability = 0x34, kCapability = 0x24, kDeviceInfo = 0x4c,
};

static void query(unsigned message)
{
    uint8_t request[268]; /* QMI header plus upstream HOST_CAP maximum */
    if (++qmi_transaction == 0) ++qmi_transaction;
    qmi_message = (uint16_t)message;
    int length = nw_wcn6750_query_request(message, qmi_transaction, request, sizeof request);
    if (length < 0 || qcom_qrtr_send_data(&router, kClientPort, qmi_node, qmi_port, request, (uint32_t)length)) {
        native_wifi_qmi_stats[0] = (uint32_t)-1;
        qmi_pending = 0;
        printf("UKE WIFI: QMI send failed message=%x encoder=%d\n", message, length);
        return;
    }
    qmi_pending = 1;
    qmi_deadline = uke_wpss_now_ms() + nw_wcn6750_query_timeout_ms();
    native_wifi_qmi_stats[0] = message == kRegister ? 6 : message == kHostCapability ? 7 : message == kCapability ? 1 : 2;
    native_wifi_qmi_stats[1]++;
    native_wifi_qmi_stats[6] = message;
    native_wifi_qmi_stats[7] = qmi_transaction;
    printf("UKE WIFI: QMI request message=%x txn=%u node=%u port=%u\n", message, qmi_transaction, qmi_node, qmi_port);
}

static void qmi_receive(void *context, QcomQrtr *q, uint32_t local_port,
                        uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    (void)context; (void)q; (void)local_port;
    QcomQmiMessage message;
    if (!qmi_pending || node != qmi_node || port != qmi_port ||
        qcom_qmi_parse(data, length, &message) || message.type != QCOM_QMI_RESPONSE ||
        message.transaction != qmi_transaction || message.message != qmi_message) {
        native_wifi_qmi_stats[4]++;
        return;
    }
    uint32_t result_bytes = 0;
    const uint8_t *result = qcom_qmi_tlv(&message, 2, &result_bytes);
    if (message.length + QCOM_QMI_HEADER != length || !result || result_bytes != 4) {
        native_wifi_qmi_stats[5]++;
        native_wifi_qmi_stats[0] = (uint32_t)-2;
        qmi_pending = 0;
        printf("UKE WIFI: QMI malformed reply message=%x txn=%u\n", qmi_message, qmi_transaction);
        return;
    }
    qmi_pending = 0;
    native_wifi_qmi_stats[2]++;
    native_wifi_identity.result = (uint16_t)(result[0] | (uint16_t)result[1] << 8);
    native_wifi_identity.error = (uint16_t)(result[2] | (uint16_t)result[3] << 8);
    native_wifi_qmi_stats[8] = native_wifi_identity.result;
    native_wifi_qmi_stats[9] = native_wifi_identity.error;
    int decoded = nw_wcn6750_query_decode(qmi_message, data, length, &native_wifi_identity);
    if (decoded < 0) {
        native_wifi_qmi_stats[0] = (uint32_t)-3;
        printf("UKE WIFI: QMI rejected message=%x result=%u error=%u decode=%d\n",
               qmi_message, native_wifi_identity.result, native_wifi_identity.error, decoded);
        return;
    }
    if (qmi_message == kRegister || qmi_message == kHostCapability) {
        printf("UKE WIFI: QMI accepted message=%x txn=%u\n", qmi_message, qmi_transaction);
        query(qmi_message == kRegister ? kHostCapability : kCapability);
    } else if (qmi_message == kCapability) {
        native_wifi_qmi_stats[0] = 4;
        printf("UKE WIFI: QMI CAP valid=%x chip=%x family=%x board=%x soc=%x fw=%x\n",
               native_wifi_identity.valid, native_wifi_identity.chip_id, native_wifi_identity.chip_family,
               native_wifi_identity.board_id, native_wifi_identity.soc_id, native_wifi_identity.fw_version);
        query(kDeviceInfo);
    } else {
        native_wifi_qmi_stats[0] = 5;
        printf("UKE WIFI: QMI BAR address=%llx size=%x valid=%x\n",
               (unsigned long long)native_wifi_identity.bar_address, native_wifi_identity.bar_size, native_wifi_identity.valid);
    }
}

void uke_wifi_qrtr_tick(void)
{
    if (qmi_pending && uke_wpss_now_ms() >= qmi_deadline) {
        qmi_pending = 0;
        native_wifi_qmi_stats[0] = (uint32_t)-4;
        native_wifi_qmi_stats[3]++;
        printf("UKE WIFI: QMI timeout message=%x txn=%u\n", qmi_message, qmi_transaction);
    }
}

uint32_t uke_wifi_qrtr_wait_ms(void)
{
    if (!qmi_pending) return 3600u * 1000u;
    uint64_t now = uke_wpss_now_ms();
    if (now >= qmi_deadline) return 1;
    return qmi_deadline - now < 1000 ? (uint32_t)(qmi_deadline - now) : 1000;
}

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
    qmi_pending = 0;
    bzero(&native_wifi_identity, sizeof native_wifi_identity);
    native_wifi_qmi_stats[0] = 0;
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
        qmi_node = router.remote[i].node;
        qmi_port = router.remote[i].port;
        query(kRegister);
        return;
    }
    if (native_wifi_qrtr_stats[0] == 3) {
        native_wifi_qrtr_stats[0] = 2;
        native_wifi_qrtr_stats[4] = native_wifi_qrtr_stats[5] = native_wifi_qrtr_stats[6] = 0;
        qmi_pending = 0;
        bzero(&native_wifi_identity, sizeof native_wifi_identity);
        native_wifi_qmi_stats[0] = 0;
        printf("UKE WIFI: WLFW service removed\n");
    }
}

void uke_wifi_qrtr_start(void)
{
    const UkeWpssClient client = {state, receive};
    qcom_qrtr_init(&router, 1, send, NULL);
    bzero(&native_wifi_identity, sizeof native_wifi_identity);
    if (qcom_qrtr_add_client(&router, kClientPort, qmi_receive, NULL)) {
        printf("UKE WIFI: QMI client registration failed\n");
        return;
    }
    /* The stock GLINK edge offers 0x800/0x2000/0x4400 receive buffers.
     * Uniform largest-size intents also admit all three message sizes. */
    channel = uke_wpss_channel("IPCRTR", 0x4400, 10, 0, &client, NULL);
    native_wifi_qrtr_stats[0] = channel ? 1 : 0;
}
