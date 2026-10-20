/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengGmuVotes.h"
#include "ShengGmuVotesSource.h"
AdrenoHfiStartupStatus ShengGmuBuildStartup(const ShengGmuVoteInputs *in,AdrenoHfiStartupConfig *out)
{
    if(!in || !out || in->chip_id!=SHENG_GMU_VOTES_CHIP_ID || in->gmxc_present>1 ||
        in->enable_acd>1 || in->enable_ifpc>1 ||
        (!in->gmxc_present && (in->gmxc.data || in->gmxc.bytes)))return AdrenoHfiStartupInvalid;
    AdrenoHfiStartupConfig c={0};uint32_t votes[16],levels[16];
    const ShengGmuAux *gpu_secondary=in->gmxc_present?&in->gmxc:&in->mx;
    for(unsigned i=0;i<SHENG_GPU_OPPS;++i)levels[i]=sheng_gpu_opps[i].level;
    if(AdrenoHfiArcVotes(levels,SHENG_GPU_OPPS,in->gfx.data,in->gfx.bytes,
        gpu_secondary->data,gpu_secondary->bytes,votes,16))return AdrenoHfiStartupInvalid;
    c.gpu_count=SHENG_GPU_OPPS;
    for(unsigned i=0;i<c.gpu_count;++i) {
        c.gpu[i]=(AdrenoHfiPerformanceLevel){sheng_gpu_opps[i].hz,votes[i]};
        if(in->enable_acd && sheng_gpu_opps[i].acd_present) {
            c.acd_enable_mask|=1u<<i;c.acd_data[c.acd_count++]=sheng_gpu_opps[i].acd;
        }
    }
    if(c.acd_count)c.acd_version=c.acd_stride=1;
    for(unsigned i=0;i<SHENG_GMU_OPPS;++i)levels[i]=sheng_gmu_opps[i].level;
    if(AdrenoHfiArcVotes(levels,SHENG_GMU_OPPS,in->cx.data,in->cx.bytes,
        in->mx.data,in->mx.bytes,votes,16))return AdrenoHfiStartupInvalid;
    c.gmu_count=SHENG_GMU_OPPS;
    for(unsigned i=0;i<c.gmu_count;++i)c.gmu[i]=(AdrenoHfiPerformanceLevel){sheng_gmu_opps[i].hz,votes[i]};
    const ShengGmuBcmInput *inputs[]={&in->sh0,&in->mc0,&in->acv};AdrenoHfiBcm bcms[3];
    for(unsigned i=0;i<3;++i) {
        bcms[i]=sheng_bcm_policy[i];bcms[i].address=inputs[i]->address;
        if(AdrenoHfiBcmAux(bcms+i,inputs[i]->aux.data,inputs[i]->aux.bytes))return AdrenoHfiStartupInvalid;
    }
    if(AdrenoHfiBuildBandwidth(sheng_gpu_bandwidths,SHENG_GPU_BANDWIDTHS,bcms,3,in->cn0_address,&c))
        return AdrenoHfiStartupInvalid;
    c.enable_ifpc=in->enable_ifpc;
    if(AdrenoHfiStartupValidate(&c))return AdrenoHfiStartupInvalid;
    *out=c;return AdrenoHfiStartupSuccess;
}
