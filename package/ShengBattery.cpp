// SPDX-License-Identifier: BSD-2-Clause-Patent
// The battery as iOS reads it (boot argument battery=1, started by BatteryProbe.cpp; needs adsp=2): an IOPMPowerSource whose
// values come from the battery manager in the audio DSP over the PMIC GLINK channel (ShengBattmgr.c). powerd finds every IOPMPowerSource,
// reads CurrentCapacity/MaxCapacity, IsCharging, ExternalConnected, voltage, amperage and AdapterDetails and listens for the status change
// message updateStatus sends; the status bar, the lock screen's charging state and Settings > Battery follow from that.
//
// DSP callbacks copy responses to a bounded ring. The service work loop owns
// requests and snapshots. The upstream IOInterruptEventSource signals that
// work loop. Retire timed-out GETs individually and retry incomplete snapshots.
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include "QcomUcsi.h"
#include "UkeDpAltMode.h"
#include "UkeUsbBoard.h"
#include "NativeExternalDisplay.h"
extern "C" {
#include "ShengAdsp.h"
#include "ShengBattmgr.h"
}

// The USB-C owner (32779) shares this channel: QcomDwc3 asks what is on the cable over UCSI.
extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *) __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));
extern "C" void qcom_dwc3_vbus(int);
extern "C" uint32_t qcom_ucsi_start(uint8_t *);
extern "C" uint32_t qcom_ucsi_receive(const uint8_t *, uint32_t, uint8_t *);
extern "C" uint32_t qcom_ucsi_tick(uint8_t *);
extern "C" bool qcom_ucsi_polling();
extern "C" void qcom_ucsi_stop();
extern "C" bool qcom_ucsi_connection(uint32_t *, uint32_t *);
extern "C" void qcom_ucsi_apply_connection(int lane);
// [0] stage (1 class up, 2 channel registered, 3 channel up, 4 published; negative: the step that failed), [1] percent, [2] mV,
// [3] mA, [4] temperature (raw), [5] flags (bit 0 present, 1 external, 2 charging, 3 full, 4 notifications enabled), [6] refresh
// cycles, [7] answers, [8] timeouts | errors << 16, [9] notifications, [10] USB type (raw), [11] full mAh, [12] design mAh,
// [13] cycles, [14] messages dropped (ring full) | ignored << 16, [15] send refusals
extern "C" {extern volatile int32_t native_battery_stats[16];}
// The raw readings behind the values: [0] capacity property (hundredths of a percent), [1] gauge 1 remaining, [2] gauge 1 full, [3] gauge 2
// remaining, [4] gauge 2 full, [5] temperature property, [6] current property (uA, discharge positive), [7] valid field bits, [8] the field
// the last timeout skipped (-1 none)
extern "C" {volatile int32_t native_battery_raw[10];}
// State callbacks, receive callbacks, inbox signals, signal returns, event actions, gated drains.
extern "C" {volatile uint32_t native_battery_dispatch[6];}
// The charger as the firmware reports it (-1 no reading): [0] USB online, [1] USB type, [2] adapter type, [3] the board's adapter type,
// [4] input current limit mA, [5] the most the port offers mA, [6] USB mV, [7] USB mA, [8] battery status, [9] charge type, [10] thermal
// level, [11] charge current mA, [12] adapter watts, [13] bits: 0 cell 1 authenticated, 1 cell 2, 2 PD adapter authenticated, 3 input
// suspended (each with bit +8 set when read), [14] limit sets | tries << 8 | port type << 16, [15] last limit result
extern "C" {volatile int32_t native_charger_stats[16];}

namespace {
constexpr uint32_t kRequestMs = 2000, kRefreshMs = 60000, kRetryMs = 5000, kSlowEvery = 10, kRing = 16, kMessage = 560;
// XM_PROP_CC_ORIENTATION in the stock Uke qti_battery_charger module.
constexpr uint32_t kCcOrientationProperty = 52, kCcTimeoutMs = 300;
constexpr unsigned kAltModeAckLimit = 32, kAltModeLogLimit = 48, kAltModeTimeoutMs = 5000;
}

// Keep the upstream interrupt producer, action dispatch and work-loop gate.
// With the arm64e member-function-thunk ABI, IOWorkLoop's automatic classifier
// compares the same checkForWork thunk for this source and IOCommandGate.
// Use XNU's explicit kActive contract, as its work-loop timer sources do.
class UkeBatteryInbox final : public IOInterruptEventSource {
    OSDeclareDefaultStructors(UkeBatteryInbox)
public:
    static UkeBatteryInbox *create(OSObject *owner, Action action) {
        UkeBatteryInbox *source = OSTypeAlloc(UkeBatteryInbox);
        if (source && !source->init(owner, action)) {
            source->release();
            return nullptr;
        }
        if (source) source->flags = (source->flags & ~kPassive) | kActive;
        return source;
    }
};
OSDefineMetaClassAndStructors(UkeBatteryInbox, IOInterruptEventSource)

class ShengBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(ShengBattery)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *request = nullptr, *refresh = nullptr;
    IOTimerEventSource *ucsiTimer = nullptr;
    IOTimerEventSource *ccTimer = nullptr;
    IOTimerEventSource *altModeTimer = nullptr;
    UkeDpAltMode::Command altModePending = UkeDpAltMode::Command::None;
    uint64_t altModeDeadline = 0;
    UkeDpAltMode::Notification altModeEvents[kAltModeAckLimit];
    unsigned altModeHead = 0, altModeCount = 0, altModeLogs = 0;
    bool altModeEnabled = false, altModeQuarantined = false, altModeAux = false;
    uint8_t ucsiOut[QCOM_UCSI_MAX_MESSAGE];
    uint32_t ucsiPending = 0, ucsiResponse = 0;
    unsigned ucsiRetries = 0;
    bool ucsiActive = false;
    bool ucsiNotifyPending = false;
    int usbOnline = -1;
    uint32_t ccGeneration = 0, ccRequestGeneration = 0;
    int ccLane = -1;
    bool ccPending = false, ccUnavailable = false;
    IOInterruptEventSource *inbox = nullptr;
    IOSimpleLock *ringLock = nullptr;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    uint32_t ringHead = 0, ringCount = 0;
    bool altModeDropped = false; // protected by ringLock
    volatile uint32_t channelUp = 0, channelChanged = 0, channelLost = 0;
    bool wasUp = false, published = false;
    uint32_t transportGeneration = 0;
    uint32_t refreshes = 0, messageLogs = 0;
    uint64_t requestDeadline = 0;
    ShengBattmgr bm;
    ShengAdspChannel *channel = nullptr;

    void updateUsbRole() {
        uint32_t generation, status;
        if (!ucsiActive || !qcom_ucsi_connection(&generation, &status)) return;
        if (!(status & (1u << 19))) {
            ccGeneration = 0;
            ccLane = -1;
            qcom_ucsi_apply_connection(-1);
            return;
        }
        if (ccGeneration == generation || !ccTimer || ccUnavailable) {
            qcom_ucsi_apply_connection(ccGeneration == generation ? ccLane : -1);
            return;
        }
        if (ccPending || bm.in_flight || bm.notification_pending) return;
        uint8_t request[SHENG_BATTMGR_REQUEST_BYTES] = {};
        ucsi_put32(request, SHENG_BATTMGR_OWNER);
        ucsi_put32(request + 4, SHENG_BATTMGR_TYPE_REQUEST);
        ucsi_put32(request + 8, SHENG_BATTMGR_OP_BOARD_GET);
        ucsi_put32(request + 16, kCcOrientationProperty);
        const int result = sheng_adsp_send(channel, request, sizeof request);
        if (result) {
            printf("[usb otg] CC query send rc=%d generation=%u\n", result, generation);
            ccUnavailable = true;
            qcom_ucsi_apply_connection(-1);
            return;
        }
        ccRequestGeneration = generation;
        ccPending = true;
        ccTimer->setTimeoutMS(kCcTimeoutMs);
    }

    static void ccAction(OSObject *owner, IOTimerEventSource *) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self || !self->ccPending) return;
        self->ccPending = false;
        // These replies have no transaction ID. Ignore late results until
        // the transport reconnects instead of assigning them to a new plug.
        self->ccUnavailable = true;
        printf("[usb otg] CC query timed out generation=%u\n", self->ccRequestGeneration);
        self->updateUsbRole();
        self->pump();
    }

    void stopUcsi() {
        ucsiTimer->cancelTimeout();
        ucsiActive = ucsiNotifyPending = false;
        ucsiPending = ucsiRetries = ucsiResponse = 0;
        usbOnline = -1;
        if (ccTimer) ccTimer->cancelTimeout();
        ccGeneration = ccRequestGeneration = 0;
        ccPending = false;
        ccLane = -1;
        qcom_ucsi_stop();
    }

    void sendUcsi(uint32_t bytes) {
        if (!bytes && ucsiNotifyPending) {
            ucsiNotifyPending = false;
            bytes = ucsi_read_request(ucsiOut);
        }
        if (bytes) {
            const int result = sheng_adsp_send(channel, ucsiOut, bytes);
            if (result) {
                if (++ucsiRetries >= 5) {
                    printf("[usb otg] UCSI send failed rc=%d bytes=%u\n", result, bytes);
                    stopUcsi();
                    return;
                }
                ucsiPending = bytes;
                ucsiTimer->setTimeoutMS(kUcsiPollMs);
                return;
            }
            ucsiResponse = ucsi_get32(ucsiOut + 8);
            ucsiPending = ucsiRetries = 0;
            ucsiTimer->setTimeoutMS(kRequestMs);
        } else if (qcom_ucsi_polling()) {
            ucsiTimer->setTimeoutMS(kUcsiPollMs);
        }
    }

    static void ucsiAction(OSObject *owner, IOTimerEventSource *) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self || !self->wasUp || !self->ucsiActive) return;
        if (self->ucsiResponse) {
            printf("[usb otg] UCSI response timed out\n");
            self->stopUcsi();
        } else {
            self->sendUcsi(self->ucsiPending ? self->ucsiPending : qcom_ucsi_tick(self->ucsiOut));
        }
    }

    void resetAltMode() {
        altModeTimer->cancelTimeout();
        altModePending = UkeDpAltMode::Command::None;
        altModeEnabled = altModeQuarantined = false;
        altModeHead = altModeCount = altModeLogs = 0;
        if (altModeAux) qcom_uke_dp_update(-1, false, false);
    }

    void quarantineAltMode(const char *cause, int result = 0) {
        if (altModeQuarantined) return;
        printf("[dp altmode] %s cmd=%x rc=%d; waiting for PMIC transport reconnect\n",
               cause, static_cast<unsigned>(altModePending), result);
        altModeTimer->cancelTimeout();
        altModePending = UkeDpAltMode::Command::None;
        altModeEnabled = false;
        altModeQuarantined = true;
        altModeCount = 0;
        if (altModeAux) qcom_uke_dp_update(-1, false, false);
    }

    void sendAltMode(UkeDpAltMode::Command command) {
        uint8_t data[UkeDpAltMode::kRequestBytes];
        UkeDpAltMode::writeRequest(data, command);
        altModePending = command;
        const int result = sheng_adsp_send(channel, data, sizeof data);
        if (result) {
            quarantineAltMode("PAN send failed", result);
            return;
        }
        clock_interval_to_deadline(kAltModeTimeoutMs, kMillisecondScale, &altModeDeadline);
        altModeTimer->setTimeoutMS(kAltModeTimeoutMs);
    }

    void receiveAltMode(const uint8_t *data, uint32_t length) {
        using UkeDpAltMode::Command;
        if (!wasUp || altModeQuarantined || __atomic_load_n(&channelLost, __ATOMIC_ACQUIRE)) return;
        if (UkeDpAltMode::isWriteResponse(data, length)) {
            if (altModePending == Command::None) return;
            if (altModePending == Command::EnableNotifications) {
                altModeEnabled = true;
                if(native_legacy_dp_debug())printf("[dp altmode] PAN notifications enabled\n");
            } else {
                const auto &event = altModeEvents[altModeHead];
                if (altModeAux && event.validRoute) {
                    if (event.lanes == 2) qcom_uke_dp_update(event.orientation, event.hpd, event.irq);
                    else {
                        if (event.lanes == 4) printf("[dp route] four-lane route unsupported\n");
                        qcom_uke_dp_update(-1, false, false);
                    }
                }
                altModeHead = (altModeHead + 1) % kAltModeAckLimit;
                --altModeCount;
            }
            altModePending = Command::None;
            altModeTimer->cancelTimeout();
        } else {
            UkeDpAltMode::Notification event;
            if (!UkeDpAltMode::decodeNotification(data, length, event)) return;
            if (altModeLogs < kAltModeLogLimit) {
                if(native_legacy_dp_debug())printf("[dp altmode] notify svid=%04x orientation=%u mux=%u pin=%u lanes=%u hpd=%u irq=%u valid=%u\n",
                       event.svid, event.orientation, event.mux, event.pin, event.lanes,
                       event.hpd, event.irq, event.validRoute);
                ++altModeLogs;
            }
            if (altModeCount == kAltModeAckLimit) {
                quarantineAltMode("PAN acknowledgment backlog overflow");
                return;
            }
            altModeEvents[(altModeHead + altModeCount) % kAltModeAckLimit] = event;
            ++altModeCount;
        }
        // The firmware ACK is a receipt for port 0. It is independent of
        // programming the PHY or delivering HPD to the display controller.
        if (altModeEnabled && altModeCount && altModePending == Command::None) {
            sendAltMode(Command::AckNotification);
        }
    }

    static void altModeAction(OSObject *owner, IOTimerEventSource *) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self || !self->wasUp) return;
        self->drainInbox();
        if (self->altModePending != UkeDpAltMode::Command::None &&
            mach_absolute_time() >= self->altModeDeadline) {
            // A late response cannot be matched to a different PAN command.
            self->quarantineAltMode("PAN response timed out");
        }
    }

    static void dspState(void *context, int up) {
        __atomic_fetch_add(&native_battery_dispatch[0], 1u, __ATOMIC_RELAXED);
        ShengBattery *self = static_cast<ShengBattery *>(context);
        IOSimpleLockLock(self->ringLock);
        self->ringCount = self->ringHead = 0;
        self->altModeDropped = false;
        IOSimpleLockUnlock(self->ringLock);
        if (!up) __atomic_store_n(&self->channelLost, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&self->channelUp, up ? 1u : 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&self->channelChanged, 1u, __ATOMIC_RELEASE);
        __atomic_fetch_add(&native_battery_dispatch[2], 1u, __ATOMIC_RELAXED);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
        __atomic_fetch_add(&native_battery_dispatch[3], 1u, __ATOMIC_RELAXED);
    }
    static void dspReceive(void *context, const uint8_t *data, uint32_t length) {
        __atomic_fetch_add(&native_battery_dispatch[1], 1u, __ATOMIC_RELAXED);
        ShengBattery *self = static_cast<ShengBattery *>(context);
        IOSimpleLockLock(self->ringLock);
        if (self->ringCount < kRing && length <= kMessage) {
            auto &slot = self->ring[(self->ringHead + self->ringCount) % kRing];
            memcpy(slot.data, data, length);
            slot.length = length;
            self->ringCount++;
        } else {
            native_battery_stats[14] = native_battery_stats[14] + 1;
            if (self->altModeTimer && UkeDpAltMode::isOwner(data, length)) self->altModeDropped = true;
        }
        IOSimpleLockUnlock(self->ringLock);
        __atomic_fetch_add(&native_battery_dispatch[2], 1u, __ATOMIC_RELAXED);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
        __atomic_fetch_add(&native_battery_dispatch[3], 1u, __ATOMIC_RELAXED);
    }

    void subscribe() {
        uint8_t enable[SHENG_BATTMGR_ENABLE_BYTES];
        const int rc = sheng_adsp_send(channel, enable, sheng_battmgr_enable_request(enable));
        if (!rc || !native_battery_stats[15])
            printf("UKE BATTERY: subscribe generation=%u send_rc=%d\n", transportGeneration, rc);
        if (rc) native_battery_stats[15] = native_battery_stats[15] + 1;
        if (!rc) {
            bm.notification_pending = 1;
            clock_interval_to_deadline(kRequestMs, kMillisecondScale, &requestDeadline);
            request->setTimeoutMS(kRequestMs);
        } else {
            request->setTimeoutMS(20);
        }
    }
    void pump() {
        uint8_t out[SHENG_BATTMGR_REQUEST_BYTES];
        if (!wasUp || bm.poisoned || !bm.enabled || ccPending) return;
        const uint32_t n = sheng_battmgr_next(&bm, out);
        if (!n) return;
        if (sheng_adsp_send(channel, out, n)) {
            bm.in_flight = 0; // A refused queue entry was never transmitted.
            request->setTimeoutMS(20);
            native_battery_stats[15] = native_battery_stats[15] + 1;
            return;
        }
        clock_interval_to_deadline(kRequestMs, kMillisecondScale, &requestDeadline);
        request->setTimeoutMS(kRequestMs);
    }
    void number(const char *key, int64_t value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32);
        if (symbol && n) setPSProperty(symbol, n);
        OSSafeReleaseNULL(n); OSSafeReleaseNULL(symbol);
    }
    void stale(bool unavailable) {
        flag("BatteryDataStale", unavailable);
        const OSSymbol *error = OSSymbol::withCString(unavailable ? "CommunicationError" : "");
        if (error) {setErrorCondition(const_cast<OSSymbol *>(error)); error->release();}
    }
    void flag(const char *key, bool value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }
    void recordRaw() {
        if (ucsiTimer && ((bm.valid >> BmUsbOnline) & 1) && usbOnline != bm.value[BmUsbOnline]) {
            usbOnline = bm.value[BmUsbOnline];
            qcom_dwc3_vbus(usbOnline);
        }
        static const int fields[8] = {BmCapacity, BmGauge1Remaining, BmGauge1Full, BmGauge2Remaining, BmGauge2Full, BmTemperature, BmCurrent, -1};
        for (unsigned i = 0; i < 7; ++i) native_battery_raw[i] = (bm.valid >> fields[i]) & 1 ? bm.value[fields[i]] : -999999;
        native_battery_raw[7] = (int32_t)bm.valid;
        native_battery_raw[8] = bm.last_timeout;
        static const int charger[13] = {BmUsbOnline, BmUsbType, BmUsbAdapter, BmRealType, BmUsbLimit, BmUsbMax, BmUsbVoltage, BmUsbCurrent,
                                        BmStatus, BmChargeType, BmThermalLevel, BmChargeCurrent, BmPowerMax};
        static const int milli[13] = {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0};
        for (unsigned i = 0; i < 13; ++i)
            native_charger_stats[i] = (bm.valid >> charger[i]) & 1 ? (milli[i] ? bm.value[charger[i]] / 1000 : bm.value[charger[i]]) : -1;
        static const int bits[4] = {BmAuthentic, BmSlaveAuthentic, BmPdVerified, BmInputSuspend};
        int32_t flags = 0;
        for (unsigned i = 0; i < 4; ++i)
            if ((bm.valid >> bits[i]) & 1) flags |= (bm.value[bits[i]] != 0) << i | 1 << (i + 8);
        native_charger_stats[13] = flags;
        native_charger_stats[14] = 0;
        native_charger_stats[15] = 0;
    }
    void publish() {
        constexpr uint32_t required = (1u << BmStatus) | (1u << BmPresent) | (1u << BmCapacity) |
            (1u << BmVoltage) | (1u << BmCurrent) | (1u << BmTemperature) | (1u << BmUsbOnline);
        printf("UKE BATTERY: refresh=%u valid=%08x required=%08x responses=%u errors=%u timeouts=%u\n",
               bm.cycles_done, bm.valid, required, bm.responses, bm.errors, bm.timeouts);
        if (bm.poisoned || (bm.valid & required) != required) {
            stale(true);
            if (!bm.poisoned) refresh->setTimeoutMS(kRetryMs);
            if (published) updateStatus();
            return;
        }
        stale(false);
        ShengBatterySnapshot s;
        sheng_battmgr_snapshot(&bm, &s);
        setBatteryInstalled(s.present);
        setExternalConnected(s.external);
        setExternalChargeCapable(s.external);
        setIsCharging(s.charging);
        setAtCriticalLevel(s.critical);
        setCurrentCapacity((unsigned)s.percent);
        setMaxCapacity(100);
        setVoltage((unsigned)(s.voltage_mv > 0 ? s.voltage_mv : 0));
        setAmperage(s.current_ma);
        if (s.cycles > 0) setCycleCount((unsigned)s.cycles);
        setTimeRemaining(s.charging ? (s.time_to_full_min >= 0 ? s.time_to_full_min : 0xffff)
                                    : (s.time_to_empty_min >= 0 ? s.time_to_empty_min : 0xffff));
        flag("FullyCharged", s.fully_charged);
        number("InstantAmperage", s.current_ma);
        if (s.temperature_valid) number("Temperature", s.temperature);
        if (s.raw_mah > 0) number("AppleRawCurrentCapacity", s.raw_mah);
        if (s.full_mah > 0) {number("AppleRawMaxCapacity", s.full_mah); number("NominalChargeCapacity", s.full_mah);}
        if (s.design_mah > 0) number("DesignCapacity", s.design_mah);
        OSDictionary *adapter = OSDictionary::withCapacity(5);
        if (adapter) {
            OSNumber *family = OSNumber::withNumber(s.adapter_family, 32);
            if (family) {adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, family); family->release();}
            if (s.external) {
                OSNumber *w = OSNumber::withNumber((unsigned long long)s.adapter_watts, 32), *mv = OSNumber::withNumber((unsigned long long)s.adapter_mv, 32),
                         *ma = OSNumber::withNumber((unsigned long long)s.adapter_ma, 32);
                if (w) {adapter->setObject(kIOPMPSAdapterDetailsWattsKey, w); w->release();}
                if (mv) {adapter->setObject(kIOPMPSAdapterDetailsVoltage, mv); mv->release();}
                if (ma) {adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, ma); ma->release();}
            }
            const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
            if (key) setPSProperty(key, adapter);
            OSSafeReleaseNULL(key);
            adapter->release();
        }
        native_battery_stats[1] = s.percent; native_battery_stats[2] = s.voltage_mv; native_battery_stats[3] = s.current_ma;
        native_battery_stats[4] = s.temperature;
        native_battery_stats[5] = s.present | s.external << 1 | s.charging << 2 | s.fully_charged << 3 | bm.enabled << 4;
        native_battery_stats[10] = (bm.valid >> BmUsbType) & 1 ? bm.value[BmUsbType] : -1;
        native_battery_stats[11] = s.full_mah; native_battery_stats[12] = s.design_mah; native_battery_stats[13] = s.cycles;
        recordRaw();
        if (!published && !s.charge_known) return;  // no charge reading yet: 0 % would read as an empty battery
        if (!published) {
            // The properties are in place before anyone finds the service.
            published = true;
            registerService();
            native_battery_stats[0] = 4;
        }
        updateStatus();
        printf("UKE BATTERY: cycle=%u raw=%x percent=%d voltage=%dmV current=%dmA temperature=%d external=%d charging=%d\n",
               bm.cycles_done, bm.valid, s.percent, s.voltage_mv, s.current_ma, s.temperature, s.external, s.charging);
    }
    void counters() {
        native_battery_stats[6] = (int32_t)bm.cycles_done;
        native_battery_stats[7] = (int32_t)bm.responses;
        native_battery_stats[8] = (int32_t)(bm.timeouts | bm.errors << 16);
        native_battery_stats[9] = (int32_t)bm.notifications;
        native_battery_stats[14] = (native_battery_stats[14] & 0xffff) | (int32_t)(bm.ignored << 16);
    }
    void handle(int result) {
        if (result == BmCycleDone) publish();
        updateUsbRole();
        pump();
        counters();
    }

    static void inboxAction(OSObject *owner, IOInterruptEventSource *, int) {
        __atomic_fetch_add(&native_battery_dispatch[4], 1u, __ATOMIC_RELAXED);
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self) {
            printf("UKE BATTERY: inbox owner class mismatch\n");
            return;
        }
        __atomic_fetch_add(&native_battery_dispatch[5], 1u, __ATOMIC_RELAXED);
        if (__atomic_load_n(&native_battery_dispatch[5], __ATOMIC_RELAXED) <= 3)
            printf("UKE BATTERY: dispatch state=%u rx=%u signal=%u returned=%u events=%u drain=%u\n",
                native_battery_dispatch[0], native_battery_dispatch[1], native_battery_dispatch[2],
                native_battery_dispatch[3], native_battery_dispatch[4], native_battery_dispatch[5]);
        if (__atomic_exchange_n(&self->channelChanged, 0u, __ATOMIC_ACQ_REL)) {
            if (__atomic_exchange_n(&self->channelLost, 0u, __ATOMIC_ACQ_REL)) {
                self->bm.valid = 0;
                self->bm.poisoned = 1;
                self->request->cancelTimeout();
                self->refresh->cancelTimeout();
                if (self->ucsiTimer) self->stopUcsi();
                if (self->altModeTimer) self->resetAltMode();
                self->stale(true);
                if (self->published) self->updateStatus();
                self->wasUp = false;
            }
            const bool up = __atomic_load_n(&self->channelUp, __ATOMIC_ACQUIRE) != 0;
            if (up && !self->wasUp) {
                self->ccUnavailable = false;
                const uint32_t generation = sheng_adsp_generation();
                if (generation != self->transportGeneration) {
                    sheng_battmgr_init(&self->bm);
                    self->transportGeneration = generation;
                    self->messageLogs = 0;
                }
                if (!self->bm.poisoned) {
                    self->subscribe();
                    sheng_battmgr_refresh(&self->bm, 1);
                    self->refresh->setTimeoutMS(kRefreshMs);
                }
                if (self->ucsiTimer) {
                    self->ucsiActive = true;
                    self->sendUcsi(qcom_ucsi_start(self->ucsiOut));
                }
                if (self->altModeTimer) self->sendAltMode(UkeDpAltMode::Command::EnableNotifications);
            }
            self->wasUp = up;
            printf("UKE BATTERY: inbox up=%u generation=%u poisoned=%u\n", (unsigned)up,
                   self->transportGeneration, self->bm.poisoned);
        }
        self->drainInbox();
    }
    // Called under the work-loop gate, also before deciding a request timed out.
    // The DSP callback may already have queued its answer while this loop slept.
    void drainInbox() {
        ShengBattery *self = this;
        for (;;) {
            uint8_t data[kMessage];
            uint32_t length = 0;
            IOSimpleLockLock(self->ringLock);
            const bool altModeDropped = self->altModeDropped;
            self->altModeDropped = false;
            if (self->ringCount) {
                auto &slot = self->ring[self->ringHead];
                length = slot.length;
                memcpy(data, slot.data, length);
                self->ringHead = (self->ringHead + 1) % kRing;
                self->ringCount--;
            }
            IOSimpleLockUnlock(self->ringLock);
            if (altModeDropped) self->quarantineAltMode("PAN message lost at inbox");
            if (!length) break;
            if (UkeDpAltMode::isOwner(data, length)) {
                if (self->altModeTimer) self->receiveAltMode(data, length);
                continue;
            }
            if (length == 24 && ucsi_get32(data) == SHENG_BATTMGR_OWNER &&
                ucsi_get32(data + 4) == SHENG_BATTMGR_TYPE_REQUEST &&
                ucsi_get32(data + 8) == SHENG_BATTMGR_OP_BOARD_GET &&
                ucsi_get32(data + 12) == kCcOrientationProperty) {
                if (self->ccPending) {
                    self->ccTimer->cancelTimeout();
                    self->ccPending = false;
                    self->ccGeneration = self->ccRequestGeneration;
                    const uint32_t orientation = ucsi_get32(data + 16), result = ucsi_get32(data + 20);
                    self->ccLane = !result && (orientation == 1 || orientation == 2) ? static_cast<int>(orientation) - 1 : -1;
                    printf("[usb otg] CC report value=%u rc=%u lane=%d generation=%u\n",
                           orientation, result, self->ccLane, self->ccGeneration);
                    self->updateUsbRole();
                    self->pump();
                }
                continue;
            }
            if (length >= 12 && ucsi_get32(data) == QCOM_UCSI_OWNER) {
                if (self->ucsiActive) {
                    const uint32_t opcode = ucsi_get32(data + 8);
                    if (opcode == kUcsiOpNotify && (self->ucsiPending || self->ucsiResponse)) {
                        self->ucsiNotifyPending = true;
                        continue;
                    }
                    if (self->ucsiPending || (opcode != kUcsiOpNotify && opcode != self->ucsiResponse)) continue;
                    if ((opcode == kUcsiOpWrite && length != 16) ||
                        (opcode == kUcsiOpRead && length != 64 && length != QCOM_UCSI_MAX_MESSAGE)) continue;
                    if (opcode != kUcsiOpNotify && ucsi_get32(data + length - 4) != 0) {
                        printf("[usb otg] UCSI transport rejected opcode=%x rc=%u\n",
                               opcode, ucsi_get32(data + length - 4));
                        self->stopUcsi();
                        continue;
                    }
                    self->ucsiTimer->cancelTimeout();
                    self->ucsiResponse = 0;
                    self->sendUcsi(qcom_ucsi_receive(data, length, self->ucsiOut));
                    self->updateUsbRole();
                }
                continue;
            }
            if (self->messageLogs < 8) {
                char hex[49] = {};
                const unsigned n = length < 24 ? length : 24;
                for (unsigned i = 0; i < n; ++i) snprintf(hex + 2 * i, 3, "%02x", data[i]);
                printf("UKE BATTERY: response bytes=%u head=%s\n", length, hex);
                ++self->messageLogs;
            }
            const int result = sheng_battmgr_receive(&self->bm, data, length);
            if (result == BmAnswer || result == BmCycleDone || result == BmEnabled) self->request->cancelTimeout();
            self->handle(result);
        }
    }
    static void requestAction(OSObject *owner, IOTimerEventSource *) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self || !self->wasUp || self->bm.poisoned) return;
        self->drainInbox();
        if (!self->bm.in_flight && !self->bm.notification_pending) {
            if (!self->bm.enabled) self->subscribe();
            else self->pump();
            return;
        }
        // Draining may have completed the old request and armed a new one. A
        // timer for the old request must not expire that new request early.
        const uint64_t now = mach_absolute_time();
        if (now < self->requestDeadline) {
            uint64_t ns = 0;
            absolutetime_to_nanoseconds(self->requestDeadline - now, &ns);
            self->request->setTimeoutMS((uint32_t)((ns + 999999) / 1000000));
            return;
        }
        const int result = sheng_battmgr_timeout(&self->bm);
        self->refresh->setTimeoutMS(kRetryMs);
        self->stale(true);
        if (self->published) self->updateStatus();
        printf("UKE BATTERY: timeout field=%d generation=%u retrying\n",
               self->bm.last_timeout, self->transportGeneration);
        self->handle(result);
        if (!self->bm.enabled) self->request->setTimeoutMS(kRetryMs);
    }
    static void refreshAction(OSObject *owner, IOTimerEventSource *sender) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self || !self->wasUp) return;
        sheng_battmgr_refresh(&self->bm, ++self->refreshes % kSlowEvery == 0);
        self->handle(BmAnswer);
        sender->setTimeoutMS(kRefreshMs);
    }

public:
    void free() override {
        if (request) request->cancelTimeout();
        if (refresh) refresh->cancelTimeout();
        if (ucsiTimer) ucsiTimer->cancelTimeout();
        if (ccTimer) ccTimer->cancelTimeout();
        if (altModeTimer) altModeTimer->cancelTimeout();
        if (loop) {
            if (inbox) loop->removeEventSource(inbox);
            if (request) loop->removeEventSource(request);
            if (refresh) loop->removeEventSource(refresh);
            if (ucsiTimer) loop->removeEventSource(ucsiTimer);
            if (ccTimer) loop->removeEventSource(ccTimer);
            if (altModeTimer) loop->removeEventSource(altModeTimer);
        }
        OSSafeReleaseNULL(inbox);
        OSSafeReleaseNULL(request);
        OSSafeReleaseNULL(refresh);
        OSSafeReleaseNULL(ucsiTimer);
        OSSafeReleaseNULL(ccTimer);
        OSSafeReleaseNULL(altModeTimer);
        OSSafeReleaseNULL(loop);
        if (ringLock) IOSimpleLockFree(ringLock);
        IOPMPowerSource::free();
    }

    bool startBattery() {
        sheng_battmgr_init(&bm);
        loop = IOWorkLoop::workLoop();
        ringLock = IOSimpleLockAlloc();
        inbox = UkeBatteryInbox::create(this, &ShengBattery::inboxAction);
        request = IOTimerEventSource::timerEventSource(this, &ShengBattery::requestAction);
        refresh = IOTimerEventSource::timerEventSource(this, &ShengBattery::refreshAction);
        if (!loop || !ringLock || !inbox || !request || !refresh ||
            loop->addEventSource(inbox) != kIOReturnSuccess || loop->addEventSource(request) != kIOReturnSuccess || loop->addEventSource(refresh) != kIOReturnSuccess)
            return false;
        if (uke_usb_otg_enabled()) {
            ucsiTimer = IOTimerEventSource::timerEventSource(this, &ShengBattery::ucsiAction);
            if (ucsiTimer && loop->addEventSource(ucsiTimer) != kIOReturnSuccess) OSSafeReleaseNULL(ucsiTimer);
            if (!ucsiTimer) printf("[usb otg] UCSI timer unavailable\n");
            if (ucsiTimer) {
                ccTimer = IOTimerEventSource::timerEventSource(this, &ShengBattery::ccAction);
                if (ccTimer && loop->addEventSource(ccTimer) != kIOReturnSuccess) OSSafeReleaseNULL(ccTimer);
            }
        }
        unsigned enableAltMode = 0;
        if (PE_parse_boot_argn("uke-dp-altmode", &enableAltMode, sizeof enableAltMode) && enableAltMode == 1) {
            altModeTimer = IOTimerEventSource::timerEventSource(this, &ShengBattery::altModeAction);
            if (altModeTimer && loop->addEventSource(altModeTimer) != kIOReturnSuccess) OSSafeReleaseNULL(altModeTimer);
            if (!altModeTimer) printf("[dp altmode] timer unavailable\n");
            unsigned enableAux = 0;
            altModeAux = altModeTimer && PE_parse_boot_argn("uke-dp-aux", &enableAux, sizeof enableAux) && enableAux == 1;
        }
        native_battery_stats[0] = 1;
        const ShengAdspClient client = {&ShengBattery::dspState, &ShengBattery::dspReceive};
        channel = sheng_adsp_channel ? sheng_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 4, 0, &client, this) : nullptr;
        if (!channel) return false;
        native_battery_stats[0] = 2;
        printf("UKE BATTERY: consumer ready dispatch=upstream-event-source-active queue=%u message=%u\n", kRing, kMessage);
        return true;
    }
};
OSDefineMetaClassAndStructors(ShengBattery, IOPMPowerSource)

// Called by BatteryProbe.cpp once this file's classes are registered.
extern "C" void sheng_battery_start(void) {
    ShengBattery *battery = OSTypeAlloc(ShengBattery);
    if (!battery || !battery->init() || !battery->attach(IOService::getPlatform())) {
        if (battery) battery->release();
        native_battery_stats[0] = -1;
        return;
    }
    if (!battery->startBattery()) {
        native_battery_stats[0] = native_battery_stats[0] > 0 ? -native_battery_stats[0] - 1 : -1;
        battery->detach(IOService::getPlatform());
        battery->release();
        return;
    }
    battery->release();  // the platform's attachment keeps it
}
