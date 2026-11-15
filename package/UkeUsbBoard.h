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
bool uke_usb_reset_controller();

// SE0 is shared with the first backlight chip. The action may use the bus only for the duration of this call.
struct GeniI2c;
using UkeWcdBusAction = bool (*)(GeniI2c &);
IOReturn uke_backlight_with_wcd_bus(UkeWcdBusAction action);
