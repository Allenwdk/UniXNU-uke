/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Follow ath11k's fixed-memory WLFW server-arrival sequence through
 * device identity discovery. The full ath11k backend will own BDF and
 * the radio lifecycle. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/systm.h>
#include "QcomQrtr.h"
#include "UkeWpss.h"
#include "nw_wcn6750_qmi.h"

extern uint64_t uke_wpss_now_ms(void);
extern void uke_wifi_probe_bar(uint64_t base, uint32_t size);
extern void uke_wifi_smmu_report(const char *when);
extern int uke_wlfw_load_stock_data(unsigned type, uint8_t **data, uint32_t *length);
extern void uke_wlfw_release_stock_data(uint8_t *data, uint32_t length);
extern void *uke_wlfw_alloc(size_t bytes, int zero);
extern void uke_wlfw_free(void *data);

static QcomQrtr router;
static UkeWpssChannel *channel;
static unsigned qmi_rx_logs, board_retries;
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
    kBoardData = 0x25, kFirmwareReady = 0x21, kFirmwareInitDone = 0x38,
    kProbeBeforeData = 1, kProbeAfterData = 2, kProbeAfterInit = 4,
};

/* All state and storage belong to the WPSS worker. */
enum { kBdfRequestBytes = 6200, kBdfPacketBytes = 6232 };
static struct {
    uint8_t *data;
    uint32_t length, offset, segment;
    unsigned type, probes;
    bool complete, init_done;
} board_data;

static void release_board_data(void)
{
    uke_wlfw_release_stock_data(board_data.data, board_data.length);
    board_data.data = NULL;
    board_data.length = board_data.offset = board_data.segment = board_data.type = 0;
}

static void start_transaction(unsigned message, unsigned state)
{
    qmi_message = (uint16_t)message;
    qmi_pending = 1;
    qmi_deadline = uke_wpss_now_ms() + nw_wcn6750_query_timeout_ms();
    native_wifi_qmi_stats[0] = state;
    native_wifi_qmi_stats[1]++;
    native_wifi_qmi_stats[6] = message;
    native_wifi_qmi_stats[7] = qmi_transaction;
}

static void query(unsigned message)
{
    uint8_t request[268]; /* QMI header plus upstream HOST_CAP maximum */
    if (++qmi_transaction == 0) ++qmi_transaction;
    int length = nw_wcn6750_query_request(message, qmi_transaction, request, sizeof request);
    if (length < 0 || qcom_qrtr_send_data(&router, kClientPort, qmi_node, qmi_port, request, (uint32_t)length)) {
        native_wifi_qmi_stats[0] = (uint32_t)-1;
        qmi_pending = 0;
        printf("UKE WIFI: QMI send failed message=%x encoder=%d\n", message, length);
        return;
    }
    start_transaction(message, message == kRegister ? 6 : message == kHostCapability ? 7 : message == kCapability ? 1 : 2);
    printf("UKE WIFI: QMI request message=%x txn=%u node=%u port=%u\n", message, qmi_transaction, qmi_node, qmi_port);
}

static void send_board_segment(void)
{
    if (!board_data.data) {
        int rc = uke_wlfw_load_stock_data(board_data.type, &board_data.data, &board_data.length);
        if (rc || !board_data.data || !board_data.length) {
            printf("UKE WIFI: stock data load failed type=%u rc=%d\n", board_data.type, rc);
            native_wifi_qmi_stats[0] = (uint32_t)-5;
            release_board_data();
            return;
        }
    }
    if (++qmi_transaction == 0) ++qmi_transaction;
    const uint32_t remaining = board_data.length - board_data.offset;
    uint8_t *workspace = uke_wlfw_alloc(kBdfRequestBytes + kBdfPacketBytes, 0);
    if (!workspace) {
        printf("UKE WIFI: board data workspace allocation failed\n");
        native_wifi_qmi_stats[0] = (uint32_t)-5;
        release_board_data();
        return;
    }
    int length = nw_wcn6750_bdf_request(qmi_transaction, board_data.type, 0xff,
                                       board_data.segment, remaining, board_data.data + board_data.offset,
                                       workspace, kBdfRequestBytes);
    int sent = length < 0 ? length : qcom_qrtr_send_data_buffered(&router, kClientPort, qmi_node, qmi_port,
                   workspace, (uint32_t)length, workspace + kBdfRequestBytes, kBdfPacketBytes);
    uke_wlfw_free(workspace);
    if (sent) {
        printf("UKE WIFI: board data send failed type=%u seg=%u encoder=%d\n",
               board_data.type, board_data.segment, length);
        native_wifi_qmi_stats[0] = (uint32_t)-1;
        release_board_data();
        return;
    }
    start_transaction(kBoardData, 8);
    printf("UKE WIFI: board data request type=%u seg=%u remaining=%u txn=%u\n",
           board_data.type, board_data.segment, remaining, qmi_transaction);
}

static void board_segment_accepted(void)
{
    uint32_t remaining = board_data.length - board_data.offset;
    board_data.offset += remaining > NW_WCN6750_BDF_CHUNK ? NW_WCN6750_BDF_CHUNK : remaining;
    ++board_data.segment;
    board_retries = 0;
    if (board_data.offset != board_data.length) return;
    unsigned type = board_data.type;
    printf("UKE WIFI: board data accepted type=%u bytes=%u segments=%u\n",
           type, board_data.length, board_data.segment);
    release_board_data();
    if (type == NW_WCN6750_REGDB) {
        board_data.type = NW_WCN6750_BDF_ELF;
    } else {
        board_data.complete = true;
        board_data.probes |= kProbeAfterData;
        if (board_data.init_done) board_data.probes |= kProbeAfterInit;
        native_wifi_qmi_stats[0] = 9;
        uke_wifi_smmu_report("data-done");
    }
}

static void qmi_receive(void *context, QcomQrtr *q, uint32_t local_port,
                        uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    (void)context; (void)q; (void)local_port;
    QcomQmiMessage message;
    if (node != qmi_node || port != qmi_port || qcom_qmi_parse(data, length, &message)) {
        native_wifi_qmi_stats[4]++;
        return;
    }
    if (message.type == QCOM_QMI_INDICATION && message.length + QCOM_QMI_HEADER == length) {
        if (message.message != kFirmwareInitDone && message.message != kFirmwareReady && qmi_rx_logs < 48) {
            qmi_rx_logs++;
            printf("UKE WIFI: QMI indication msg=%x len=%u\n", message.message, length);
        }
        if (message.message == kFirmwareInitDone && !board_data.init_done) {
            board_data.init_done = true;
            if (board_data.complete) board_data.probes |= kProbeAfterInit;
            printf("UKE WIFI: WLFW FW_INIT_DONE\n");
        } else if (message.message == kFirmwareReady) {
            printf("UKE WIFI: WLFW FW_READY\n");
        }
        return;
    }
    if (!qmi_pending || message.type != QCOM_QMI_RESPONSE ||
        message.transaction != qmi_transaction || message.message != qmi_message) {
        if (qmi_rx_logs < 48) {
            qmi_rx_logs++;
            printf("UKE WIFI: QMI unmatched type=%x msg=%x txn=%u len=%u pending=%d\n",
                   message.type, message.message, message.transaction, length, qmi_pending);
        }
        native_wifi_qmi_stats[4]++;
        return;
    }
    uint32_t result_bytes = 0;
    const uint8_t *result = qcom_qmi_tlv(&message, 2, &result_bytes);
    if (message.length + QCOM_QMI_HEADER != length || !result || result_bytes != 4) {
        native_wifi_qmi_stats[5]++;
        native_wifi_qmi_stats[0] = (uint32_t)-2;
        qmi_pending = 0;
        release_board_data();
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
        release_board_data();
        return;
    }
    if (qmi_message == kBoardData) {
        board_segment_accepted();
    } else if (qmi_message == kRegister || qmi_message == kHostCapability) {
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
        board_data.probes |= kProbeBeforeData;
        const uint32_t required = NW_WCN6750_CHIP | NW_WCN6750_SOC | NW_WCN6750_BAR;
        if ((native_wifi_identity.valid & required) == required &&
            !(native_wifi_identity.valid & NW_WCN6750_BOARD) &&
            native_wifi_identity.chip_id == 1 && native_wifi_identity.chip_family == 0xb &&
            native_wifi_identity.soc_id == 0x40140120 &&
            native_wifi_identity.bar_address == 0x61e00000 && native_wifi_identity.bar_size == 0x200000)
            board_data.type = NW_WCN6750_REGDB;
        else
            printf("UKE WIFI: stock board data skipped for unmatched identity\n");
    }
}

void uke_wifi_qrtr_tick(void)
{
    if (qmi_pending && uke_wpss_now_ms() >= qmi_deadline) {
        qmi_pending = 0;
        native_wifi_qmi_stats[3]++;
        printf("UKE WIFI: QMI timeout message=%x txn=%u\n", qmi_message, qmi_transaction);
        uke_wifi_smmu_report("timeout");
        if (qmi_message == kBoardData && board_data.data && board_retries < 1) {
            board_retries++;
            printf("UKE WIFI: board data retry type=%u seg=%u txn=%u\n",
                   board_data.type, board_data.segment, qmi_transaction);
        } else {
            release_board_data();
            native_wifi_qmi_stats[0] = (uint32_t)-4;
        }
    }
    if (board_data.probes) {
        const unsigned phase = board_data.probes;
        board_data.probes = 0;
        printf("UKE WIFI: MMIO phase=%x\n", phase);
        uke_wifi_probe_bar(native_wifi_identity.bar_address, native_wifi_identity.bar_size);
    }
    if (!qmi_pending && board_data.type) send_board_segment();
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
    release_board_data();
    bzero(&board_data, sizeof board_data);
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
        release_board_data();
        bzero(&board_data, sizeof board_data);
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
