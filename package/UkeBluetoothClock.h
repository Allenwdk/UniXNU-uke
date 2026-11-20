/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef UKE_BLUETOOTH_CLOCK_H
#define UKE_BLUETOOTH_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct uke_bluetooth_clock_io {
    void *context;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    void (*delay_us)(void *,unsigned);
};
/* UART engine is stopped before source changes. Cliffs TCXO is 19.2 MHz and GPLL0-even is 300 MHz. */
int uke_bluetooth_clock_source(const struct uke_bluetooth_clock_io *,uint32_t baud,uint32_t *hz);
bool uke_bluetooth_clock_vote(volatile uint32_t *,bool enabled);
#ifdef __cplusplus
}
#endif
#endif
