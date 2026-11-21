// SPDX-License-Identifier: BSD-2-Clause-Patent
// ArmCpu board adaptation of sheng's sleep controller for Uke's PMIC PON.
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include "ArmCpuSleep.h"

extern "C" int armcpu_sleep_register(const struct armcpu_sleep_board *);
extern "C" volatile struct armcpu_sleep_stats armcpu_sleep_stats;
extern "C" uint32_t uke_buttons_sleep_intid(void);
extern "C" bool uke_buttons_sleep_armed(void);
extern "C" bool uke_buttons_sleep_check(void);
extern "C" void uke_gunyah_wdt_sleep_tick(void);
extern "C" void uke_sleep_backlight(unsigned message);
extern "C" void uke_wifi_ahb_wake(void);

namespace {
bool gStarted, gRegistered;
bool gPowerWake, gPromoteWake;
uint32_t gWakeIntid;

// These three ArmCpu callbacks execute without normal driver scheduling.
void prepare() {
    __atomic_store_n(&gPowerWake, false, __ATOMIC_RELAXED);
    __atomic_store_n(&gPromoteWake, false, __ATOMIC_RELAXED);
}

int shouldWake(uint32_t intid) {
    if (intid != gWakeIntid || !uke_buttons_sleep_check()) return 0;
    __atomic_store_n(&gPowerWake, true, __ATOMIC_RELEASE);
    __atomic_store_n(&gPromoteWake, true, __ATOMIC_RELEASE);
    return 1;
}

const struct armcpu_sleep_board kBoard = {
    prepare, shouldWake, nullptr, &gWakeIntid, 1, uke_gunyah_wdt_sleep_tick, 10
};
static_assert(sizeof(kBoard) == ARMCPU_SLEEP_BOARD_SIZE);
static_assert(sizeof(armcpu_sleep_stats) == ARMCPU_SLEEP_STATS_SIZE);

void promotePowerWake(IOPMrootDomain *root) {
    if (root && __atomic_exchange_n(&gPromoteWake, false, __ATOMIC_ACQ_REL))
        root->requestUserActive(root, "Uke power key");
}

IOReturn sleepWake(void *, void *, UInt32 message, IOService *, void *, vm_size_t) {
    switch (message) {
    case kIOMessageSystemWillSleep:
        IOLog("UKE SLEEP: will sleep power_key_armed=%u\n", uke_buttons_sleep_armed());
        uke_sleep_backlight(message);
        break;
    case kIOMessageSystemWillNotSleep:
        uke_sleep_backlight(message);
        break;
    case kIOMessageSystemWillPowerOn: {
        IOPMrootDomain *root = IOService::getPMRootDomain();
        if (root && __atomic_load_n(&gPowerWake, __ATOMIC_ACQUIRE))
            root->claimSystemWakeEvent(root, kIOPMWakeEventSource, "Uke power key");
        uke_sleep_backlight(message);
        promotePowerWake(root);
        break;
    }
    case kIOMessageSystemHasPoweredOn: {
        const auto &stats = armcpu_sleep_stats;
        uint64_t rate;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(rate));
        const uint64_t durationMs = rate ? stats.asleep_ticks * 1000 / rate : 0;
        IOLog("UKE SLEEP: resumed power=%u reason=%u intid=%u ms=%llu entries=%u ticks=%u spurious=%u masked=%u unarmed=%u cpu_off_failures=%u phase=%u\n",
              __atomic_load_n(&gPowerWake, __ATOMIC_ACQUIRE), stats.last_reason, stats.last_intid,
              durationMs, stats.entries, stats.ticks, stats.spurious, stats.masked, stats.unarmed,
              stats.cpu_off_failures, stats.phase);
        uke_sleep_backlight(message);
        uke_wifi_ahb_wake();
        promotePowerWake(IOService::getPMRootDomain());
        break;
    }
    default:
        break;
    }
    return kIOReturnSuccess;
}

void install(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "uke sleep");
    IONotifier *interest = nullptr;
    int result = kIOReturnNotReady;
    for (unsigned attempt = 0; attempt < 240; ++attempt) {
        if (uke_buttons_sleep_armed() && IOService::getPMRootDomain()) {
            if (!interest) interest = registerSleepWakeInterest(sleepWake, nullptr, nullptr);
            if (interest) {
                result = armcpu_sleep_register(&kBoard);
                if (!result) {
                    __atomic_store_n(&gRegistered, true, __ATOMIC_RELEASE);
                    IOLog("UKE SLEEP: wake source ready power_intid=%u\n", gWakeIntid);
                    return;
                }
            }
        }
        IOSleep(250);
    }
    if (interest) interest->remove();
    IOLog("UKE SLEEP: wake source unavailable rc=0x%x power_key_armed=%u\n",
          result, uke_buttons_sleep_armed());
}
} // namespace

extern "C" bool uke_platform_sleep_ready(void) {
    return __atomic_load_n(&gRegistered, __ATOMIC_ACQUIRE) && uke_buttons_sleep_armed();
}

extern "C" void uke_sleep_promote_power_wake(void) {
    if (__atomic_load_n(&gPromoteWake, __ATOMIC_ACQUIRE))
        promotePowerWake(IOService::getPMRootDomain());
}

extern "C" void uke_sleep_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("platform-sleep", &enabled, sizeof(enabled)) || enabled != 1 ||
        __atomic_exchange_n(&gStarted, true, __ATOMIC_ACQ_REL)) return;
    gWakeIntid = uke_buttons_sleep_intid();
    thread_t thread;
    const kern_return_t result = kernel_thread_start(install, nullptr, &thread);
    if (result == KERN_SUCCESS) thread_deallocate(thread);
    else IOLog("UKE SLEEP: registration thread failed rc=0x%x\n", result);
}
