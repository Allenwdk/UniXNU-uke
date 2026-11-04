/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef UKE_GPU_DCVS_OPPS_H
#define UKE_GPU_DCVS_OPPS_H
#include "AdrenoHfiStartup.h"
/* Use the same zero/off + ascending OPP order that was sent to the GMU.
 * Reject malformed tables before starting the governor or indexing its clocks. */
struct GpuDcvsOpps {
    uint32_t mhz[16]{};
    unsigned top=0;
    bool load(const AdrenoHfiStartupConfig &config) {
        if(config.gpu_count<2 || config.gpu_count>16 || config.gpu[0].hz)return false;
        for(unsigned i=1;i<config.gpu_count;++i) {
            const uint64_t hz=config.gpu[i].hz;
            if(!hz || hz%1000000 || hz/1000000>UINT32_MAX || hz<=config.gpu[i-1].hz)return false;
            mhz[i]=uint32_t(hz/1000000);
        }
        top=config.gpu_count-1;return true;
    }
    unsigned levelFor(uint64_t need) const {
        unsigned level=1;while(level<top && mhz[level]<need)++level;return level;
    }
    unsigned clamp(unsigned level) const {return level<1?1:level>top?top:level;}
};
#endif
