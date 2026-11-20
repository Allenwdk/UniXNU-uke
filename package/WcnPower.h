/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef UKE_WCN_POWER_H
#define UKE_WCN_POWER_H
#ifdef __cplusplus
extern "C" {
#endif
enum { UKE_WCN_WLAN=0, UKE_WCN_BLUETOOTH=1 };
/* One ownership vote per radio, idempotent until put. Return positive errno. */
int uke_wcn_power_get(unsigned radio);
int uke_wcn_power_put(unsigned radio);
unsigned uke_wcn_power_owners(void);
#ifdef __cplusplus
}
#endif
#endif
