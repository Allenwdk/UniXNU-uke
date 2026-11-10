/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* PMIC GLINK battery codec, derived from UniXNU-sheng 6bc0ce1e.
 * Uke publishes measurements and notification subscriptions only. A timed-out
 * untagged request quarantines this transport generation until a clean restart. */
#ifndef SHENG_BATTMGR_H
#define SHENG_BATTMGR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SHENG_BATTMGR_OWNER 32778u  /* the battery manager; 32779 is the USB-C port manager on the same channel */
#define SHENG_BATTMGR_TYPE_REQUEST 1u
#define SHENG_BATTMGR_TYPE_NOTIFY 2u
#define SHENG_BATTMGR_OP_REQUEST_NOTIFICATION 0x04u
#define SHENG_BATTMGR_OP_NOTIFICATION 0x07u
#define SHENG_BATTMGR_OP_BATTERY_GET 0x30u
#define SHENG_BATTMGR_OP_USB_GET 0x32u
#define SHENG_BATTMGR_OP_BOARD_GET 0x50u  /* the board's own properties, same request and answer shape: fuel gauges, charger state */
#define SHENG_BATTMGR_REQUEST_BYTES 24u
#define SHENG_BATTMGR_ENABLE_BYTES 28u

/* The fields one refresh reads, in order (at most 32: valid has a bit per field). */
enum {
    BmStatus, BmPresent, BmCapacity, BmVoltage, BmCurrent, BmTemperature, BmCharge, BmUsbOnline, BmUsbType, BmUsbVoltage,
    BmUsbCurrent, BmTimeToEmpty, BmTimeToFull, BmGauge1Remaining, BmGauge1Full, BmGauge2Remaining, BmGauge2Full, BmFull, BmDesign,
    BmCycles,
    /* the charger's state: port type as charger detection saw it, input current limit and the most the port offers (uA), charge type,
     * thermal level, the charge current the charger runs, the board's view of the adapter (type, watts, PD authenticated), battery
     * authentication of both cells, input suspended */
    BmUsbAdapter, BmUsbLimit, BmUsbMax, BmChargeType, BmThermalLevel, BmChargeCurrent, BmRealType, BmPowerMax, BmPdVerified,
    BmAuthentic, BmSlaveAuthentic, BmInputSuspend, BmFields
};

typedef struct {
    int32_t value[BmFields];
    uint32_t valid;               /* bit per field */
    uint8_t cursor, in_flight, enabled, slow_due;
    uint32_t cycles_done, responses, notifications, errors, timeouts, ignored;
    uint32_t refresh_wanted;      /* a notification asked for a new cycle while one ran */
    int last_timeout;             /* the field the last timeout skipped, -1 none */
    uint8_t poisoned, notification_pending;

} ShengBattmgr;

/* What a power source shows. Temperatures are hundredths of a degree Celsius, as the firmware reports them. The charge comes from the
 * firmware capacity property. Current is converted from discharge-positive to charge-positive. */
typedef struct {
    int present, external, charging, fully_charged, critical;
    int percent;                  /* 0..100 */
    int charge_known;             /* percent comes from a reading, not a default */
    int voltage_mv, current_ma;   /* current: positive while the battery takes charge */
    int temperature, temperature_valid;  /* valid: a reading in the range a battery can have */
    int raw_mah, full_mah, design_mah, cycles;
    int time_to_empty_min, time_to_full_min;  /* -1 unknown */
    uint32_t adapter_family;      /* kIOPSFamilyCode value, 0 when no adapter */
    int adapter_mv, adapter_ma, adapter_watts;
} ShengBatterySnapshot;

enum {BmIgnored = 0, BmAnswer = 1, BmCycleDone = 2, BmNotified = 3, BmEnabled = 4};

void sheng_battmgr_init(ShengBattmgr *b);
/* The notification enable request (sent once the channel is up). */
uint32_t sheng_battmgr_enable_request(uint8_t out[SHENG_BATTMGR_ENABLE_BYTES]);
/* Starts a refresh cycle (no effect while one runs: it is remembered and runs after). slow: also the fields that hardly change. */
void sheng_battmgr_refresh(ShengBattmgr *b, int slow);
/* Consume BmCycleDone before calling next again. A queued refresh begins here, after the owner publishes the completed snapshot.
 * Returns 0 while another request is outstanding or the transport is quarantined. */
uint32_t sheng_battmgr_next(ShengBattmgr *b, uint8_t out[SHENG_BATTMGR_REQUEST_BYTES]);
/* Quarantine an untagged request after timeout. A clean transport generation must reinitialize it. */
void sheng_battmgr_timeout(ShengBattmgr *b);
/* A message from the channel; returns one of the Bm* results. */
int sheng_battmgr_receive(ShengBattmgr *b, const uint8_t *data, uint32_t length);
/* The power source values from the fields read so far. */
void sheng_battmgr_snapshot(const ShengBattmgr *b, ShengBatterySnapshot *s);

#ifdef __cplusplus
}
#endif
#endif
