/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef UKE_GPU_SYSTEM_CACHE_H
#define UKE_GPU_SYSTEM_CACHE_H
#include "Sm7675Llcc.h"
/* Rooted uke DT: cache-controller@25000000, qcom,cliffs7-llcc + v50.
 * OEM llcc-qcom.c cliffs7_data: GPU9/3200KiB, GPUHTW11/256KiB.
 * Board values are checked against board.toml by check-uke-llcc.py. */
static constexpr uint64_t UkeLlccBase=0x25000000,UkeLlccBroadcast=0x25800000;
static constexpr uint32_t UkeLlccBytes=0x800000,UkeLlccBroadcastBytes=0x200000;
static constexpr unsigned UkeLlccDefaultMode=2;
static constexpr Sm7675LlccPolicy UkeLlccPolicy{{9,3200,0},{11,256,1}};
#endif
