// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <kern/locks.h>
#include <IOKit/IOTypes.h>

extern lck_mtx_t uke_apps_smmu_mutex;
class AppsSmmuLock {
public:
    AppsSmmuLock() {lck_mtx_lock(&uke_apps_smmu_mutex);}
    ~AppsSmmuLock() {lck_mtx_unlock(&uke_apps_smmu_mutex);}
    bool ready() const {return true;}
    AppsSmmuLock(const AppsSmmuLock &) = delete;
    AppsSmmuLock &operator=(const AppsSmmuLock &) = delete;
};

int uke_usb2_hardware_init();
bool uke_usb3_hardware_init();
bool uke_usb3_set_orientation(int lane, bool displayPort = false);
bool uke_usb_dp_route_aux(int lane);
enum class UkeDpActivity { Hold, Attention, Disconnect };
using UkeDpWait = UkeDpActivity (*)(void *);
void uke_usb_dp_read_dpcd(int lane, UkeDpWait wait, void *context);
extern "C" void qcom_uke_dp_update(int lane, bool hpd, bool irq);
bool uke_usb_reset_controller();
bool uke_usb_set_repeater_host(bool host, bool restartPhy);
bool uke_usb_otg_enabled();
struct UkeUsbOrientation {
    uint32_t gpioConfig, gpioIO, typecControl, typecStatus, pcsStatus;
};
bool uke_usb3_read_orientation(UkeUsbOrientation &state);
void uke_usb2_log_phy();

// SE0 is shared with the first backlight chip. The action may use the bus only for the duration of this call.
struct GeniI2c;
using UkeWcdBusAction = bool (*)(GeniI2c &, void *);
IOReturn uke_backlight_with_wcd_bus(UkeWcdBusAction action, void *context = nullptr);
