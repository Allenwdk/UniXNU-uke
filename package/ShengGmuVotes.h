/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_GMU_VOTES_H
#define SHENG_GMU_VOTES_H
#include "AdrenoHfiVotes.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {const uint8_t *data;size_t bytes;} ShengGmuAux;
typedef struct {uint32_t address;ShengGmuAux aux;} ShengGmuBcmInput;
typedef struct {
    uint32_t chip_id;
    ShengGmuAux gfx,mx,cx,gmxc;
    unsigned gmxc_present; /* 0=authoritatively absent, 1=present; unknown must refuse */
    ShengGmuBcmInput sh0,mc0,acv;
    uint32_t cn0_address;
    unsigned enable_acd,enable_ifpc;
} ShengGmuVoteInputs;
/* Build only CPU-side configuration from pinned board OPP/catalog policy and
 * actual cmd-db data supplied by the platform. No device read/write occurs.
 * Missing GMxC data must not silently become absent: discovery errors defer
 * this call. Caller authenticates chip/board/rail/resource ownership and must
 * establish AOSS ACD/IFPC prerequisites before ColdStart, if requested here.
 * Target pinned DT has no OPP speed-bin masks; every enabled OPP is retained.
 * All input/aux/output storage is disjoint and immutable during the call.
 * Failure leaves output unchanged. Software fixtures are not board evidence. */
AdrenoHfiStartupStatus ShengGmuBuildStartup(const ShengGmuVoteInputs *,AdrenoHfiStartupConfig *);
#ifdef __cplusplus
}
#endif
#endif
