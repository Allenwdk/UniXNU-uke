/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production observer + original Linux codec, with only the GLINK peer and
 * clock modeled. These checks do not claim any hardware QMI result. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "QcomQrtr.h"
#include "UkeWpss.h"
#include "nw_wcn6750_qmi.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "QMI check failed line%d: %s\n", __LINE__, #x); exit(1); } } while (0)
extern void uke_wifi_qrtr_start(void), uke_wifi_qrtr_tick(void);
extern volatile uint32_t native_wifi_qmi_stats[10];
extern struct nw_wcn6750_identity native_wifi_identity;
static UkeWpssClient client;
static uint64_t now;
static uint8_t outgoing[512];
static unsigned outgoing_size, requests, announced;
static unsigned bar_probes;

void uke_wifi_probe_bar(uint64_t base, uint32_t size)
{
    CHECK(base == 0x61e00000 && size == 0x200000);
    ++bar_probes;
}

void *lx_alloc(size_t n, int zero) { return zero ? calloc(1, n) : malloc(n); }
void lx_free(const void *p) { free((void *)p); }
void lx_log(int level, const char *format, ...) { (void)level; (void)format; }
uint64_t uke_wpss_now_ms(void) { return now; }

static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (unsigned n = 0; n < 4; ++n) p[n] = (uint8_t)(v >> (8 * n)); }
UkeWpssChannel *uke_wpss_channel(const char *name, uint32_t size, uint32_t count, int initiator, const UkeWpssClient *callbacks, void *context)
{
    (void)size; (void)count; (void)context;
    CHECK(!strcmp(name, "IPCRTR") && !initiator);
    client = *callbacks;
    return (UkeWpssChannel *)(uintptr_t)1;
}
int uke_wpss_send(UkeWpssChannel *channel, const void *data, uint32_t length)
{
    CHECK(channel && length <= sizeof outgoing);
    const uint8_t *bytes = data;
    if (get32(bytes + 4) == QCOM_QRTR_NEW_SERVER) ++announced;
    if (get32(bytes + 4) == QCOM_QRTR_DATA) {
        memcpy(outgoing, data, length); outgoing_size = length; ++requests;
    }
    return 0;
}
static void inject(uint32_t type, uint32_t source_port, uint32_t destination_port, const uint8_t *payload, unsigned length)
{
    uint8_t packet[512] = {0};
    CHECK(length + 32 <= sizeof packet);
    put32(packet, 1); put32(packet + 4, type); put32(packet + 8, 13); put32(packet + 12, source_port);
    put32(packet + 20, length); put32(packet + 24, 1); put32(packet + 28, destination_port);
    memcpy(packet + 32, payload, length);
    client.receive(NULL, packet, length + 32);
}
static void server(void)
{
    uint8_t packet[20] = {0};
    put32(packet, QCOM_QRTR_NEW_SERVER); put32(packet + 4, 0x45); put32(packet + 8, 0x301);
    put32(packet + 12, 13); put32(packet + 16, 8);
    inject(QCOM_QRTR_NEW_SERVER, QCOM_QRTR_PORT_CTRL, QCOM_QRTR_PORT_CTRL, packet, sizeof packet);
}
static unsigned transaction(void) { CHECK(outgoing_size >= 39); return outgoing[33] | (unsigned)outgoing[34] << 8; }
static unsigned operation(void) { return outgoing[35] | (unsigned)outgoing[36] << 8; }
static unsigned result_header(uint8_t *packet, unsigned message, unsigned txn, unsigned result, unsigned error)
{
    memset(packet, 0, 256); packet[0] = QCOM_QMI_RESPONSE;
    put16(packet + 1, txn); put16(packet + 3, message);
    packet[7] = 2; put16(packet + 8, 4); put16(packet + 10, result); put16(packet + 12, error);
    return 14;
}
static void respond(uint8_t *packet, unsigned length)
{
    put16(packet + 5, length - 7);
    inject(QCOM_QRTR_DATA, 8, 0x4000, packet, length);
}
static void accept(unsigned message)
{
    uint8_t response[256];
    CHECK(operation() == message);
    unsigned length = result_header(response, message, transaction(), 0, 0);
    respond(response, length);
}
int main(void)
{
    uke_wifi_qrtr_start(); client.state(NULL, 1); server();
    CHECK(!announced && requests == 1 && operation() == 0x20);
    const uint8_t register_tlvs[] = {
        0x10,1,0,1, 0x15,4,0,0x4c,0x45,0x4e,0x4b, 0x18,1,0,1, 0x1b,1,0,1,
    };
    const uint8_t host_tlvs[] = {
        0x10,4,0,1,0,0,0, 0x14,1,0,1, 0x1a,1,0,0, 0x1c,1,0,0,
    };
    CHECK(get32(outgoing + 20) == 7 + sizeof register_tlvs);
    CHECK(memcmp(outgoing + 39, register_tlvs, sizeof register_tlvs) == 0);
    accept(0x20);
    CHECK(operation() == 0x34 && get32(outgoing + 20) == 7 + sizeof host_tlvs);
    CHECK(memcmp(outgoing + 39, host_tlvs, sizeof host_tlvs) == 0);
    accept(0x34);
    CHECK(requests == 3 && operation() == 0x24);
    uint8_t response[256];
    unsigned txn = transaction(), n = result_header(response, 0x24, txn + 1, 0, 0);
    respond(response, n); CHECK(requests == 3 && native_wifi_qmi_stats[4] == 1);
    n = result_header(response, 0x24, txn, 0, 0);
    response[n++] = 0x10; put16(response + n, 8); n += 2; put32(response + n, 0x6750); n += 4; put32(response + n, 0xb); n += 4;
    response[n++] = 0x11; put16(response + n, 4); n += 2; put32(response + n, 7); n += 4;
    response[n++] = 0x12; put16(response + n, 4); n += 2; put32(response + n, 0x7675); n += 4;
    respond(response, n);
    CHECK(requests == 4 && operation() == 0x4c && native_wifi_identity.board_id == 7 && native_wifi_identity.chip_id == 0x6750);
    n = result_header(response, 0x4c, transaction(), 0, 0);
    response[n++] = 0x10; put16(response + n, 8); n += 2; put32(response + n, 0x61e00000); put32(response + n + 4, 0); n += 8;
    response[n++] = 0x11; put16(response + n, 4); n += 2; put32(response + n, 0x200000); n += 4;
    respond(response, n);
    CHECK(native_wifi_qmi_stats[0] == 5 && (native_wifi_identity.valid & NW_WCN6750_BAR));
    CHECK(native_wifi_identity.bar_address == 0x61e00000 && native_wifi_identity.bar_size == 0x200000);
    CHECK(bar_probes == 1);
    client.state(NULL, 0); CHECK(native_wifi_identity.valid == 0);
    client.state(NULL, 1); server(); now = 10001; uke_wifi_qrtr_tick();
    CHECK((int32_t)native_wifi_qmi_stats[0] == -4 && native_wifi_qmi_stats[3] == 1);
    client.state(NULL, 0); client.state(NULL, 1); server();
    n = result_header(response, 0x20, transaction(), 0, 0); put16(response + 8, 3); respond(response, n);
    CHECK((int32_t)native_wifi_qmi_stats[0] == -2 && native_wifi_qmi_stats[5] == 1);
    client.state(NULL, 0); client.state(NULL, 1); server();
    accept(0x20);
    unsigned requests_before_error = requests;
    n = result_header(response, 0x34, transaction(), 1, 90); respond(response, n);
    CHECK((int32_t)native_wifi_qmi_stats[0] == -3 && native_wifi_qmi_stats[8] == 1 &&
          native_wifi_qmi_stats[9] == 90 && requests == requests_before_error);
    client.state(NULL, 0); client.state(NULL, 1); server();
    accept(0x20); accept(0x34);
    n = result_header(response, 0x24, transaction(), 0, 0);
    response[n++] = 0x16; put16(response + n, 1); n += 2; response[n++] = 0;
    respond(response, n);
    CHECK((int32_t)native_wifi_qmi_stats[0] == -3 && native_wifi_identity.valid == 0);
    CHECK(bar_probes == 1);
    CHECK(!announced);
    puts("PASS: upstream registration/host-cap wire fields, gated CAP/BAR, stale replies, timeout, malformed TLVs and firmware errors");
    return 0;
}
