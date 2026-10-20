/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengGmuVotes.h"
#include "ShengGmuVotesSource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"vote line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#ifdef VOTES_SOURCE_ORACLE
extern int source_arc(const uint32_t *,unsigned,const uint8_t *,size_t,const uint8_t *,size_t,uint32_t *);
extern int source_bandwidth(const uint32_t *,unsigned,const AdrenoHfiBcm *,unsigned,uint32_t,uint32_t [159]);
#endif
void test_sheng_votes(AdrenoHfiStartupConfig *full)
{
    /* Synthetic rail/BCM data for software only, never physical addresses. */
    uint8_t gfx[]={0,0,64,0,128,0,192,0,0,1,64,1,128,1,192,1,0,2};
    uint8_t mx[]={0,0,64,0,0,1,128,1,0,2},gmxc[]={0,0,128,0,128,1};
    uint8_t sh0[]={0xe8,3,0,0,16,0,0,0},mc0[]={0xe8,3,0,0,4,0,1,0},acv[]={0,0,0,0,0,0,1,0};
    ShengGmuVoteInputs in={.chip_id=SHENG_GMU_VOTES_CHIP_ID,.gfx={gfx,sizeof(gfx)},.cx={gfx,sizeof(gfx)},.mx={mx,sizeof(mx)},
        .sh0={0x1000,{sh0,8}},.mc0={0x1004,{mc0,8}},.acv={0x1008,{acv,8}},.cn0_address=0x2000,.enable_acd=1,.enable_ifpc=1};
    AdrenoHfiStartupConfig c;
    for(unsigned present=0;present<2;++present)for(unsigned acd_enabled=0;acd_enabled<2;++acd_enabled)for(unsigned ifpc=0;ifpc<2;++ifpc) {
        in.gmxc_present=present;in.gmxc=present?(ShengGmuAux){gmxc,sizeof(gmxc)}:(ShengGmuAux){0};
        in.enable_acd=acd_enabled;in.enable_ifpc=ifpc;
        CHECK(ShengGmuBuildStartup(&in,&c)==AdrenoHfiStartupSuccess);
        CHECK(c.gpu_count==SHENG_GPU_OPPS && c.gmu_count==SHENG_GMU_OPPS && c.bandwidth_count==SHENG_GPU_BANDWIDTHS && c.enable_ifpc==ifpc);
        unsigned packed=0,mask=0;
        for(unsigned i=0;i<c.gpu_count;++i) {
            CHECK(c.gpu[i].hz==sheng_gpu_opps[i].hz);
            if(acd_enabled && sheng_gpu_opps[i].acd_present){CHECK(c.acd_data[packed++]==sheng_gpu_opps[i].acd);mask|=1u<<i;}
        }
        CHECK(c.acd_count==packed && c.acd_enable_mask==mask);
#ifdef VOTES_SOURCE_ORACLE
        uint32_t levels[16],expected[16],wire[159],actual[159],id;size_t words;
        for(unsigned i=0;i<c.gpu_count;++i)levels[i]=sheng_gpu_opps[i].level;
        const ShengGmuAux *secondary=present?&in.gmxc:&in.mx;
        CHECK(!source_arc(levels,c.gpu_count,in.gfx.data,in.gfx.bytes,secondary->data,secondary->bytes,expected));
        for(unsigned i=0;i<c.gpu_count;++i)CHECK(c.gpu[i].vote==expected[i]);
        for(unsigned i=0;i<c.gmu_count;++i)levels[i]=sheng_gmu_opps[i].level;
        CHECK(!source_arc(levels,c.gmu_count,in.cx.data,in.cx.bytes,in.mx.data,in.mx.bytes,expected));
        for(unsigned i=0;i<c.gmu_count;++i)CHECK(c.gmu[i].vote==expected[i] && c.gmu[i].hz==sheng_gmu_opps[i].hz);
        AdrenoHfiBcm bcms[3];const ShengGmuBcmInput *b[]={&in.sh0,&in.mc0,&in.acv};
        for(unsigned i=0;i<3;++i){bcms[i]=sheng_bcm_policy[i];bcms[i].address=b[i]->address;CHECK(!AdrenoHfiBcmAux(bcms+i,b[i]->aux.data,b[i]->aux.bytes));}
        CHECK(!source_bandwidth(sheng_gpu_bandwidths,SHENG_GPU_BANDWIDTHS,bcms,3,in.cn0_address,wire));
        CHECK(!AdrenoHfiStartupEncode(&c,AdrenoHfiStageBandwidth,&id,actual,159,&words) && words==159 && !memcmp(wire,actual,sizeof(wire)));
#endif
    }
    *full=c; /* Last profile has GMxC, ACD and IFPC enabled. */
    ShengGmuVoteInputs good=in;
    for(unsigned control=0;control<13;++control) {
        in=good;
        switch(control) {
        case 0:in.chip_id=0;break;case 1:in.gfx.bytes=0;break;case 2:in.cx.data=NULL;break;
        case 3:in.mx.bytes=1;break;case 4:in.gmxc_present=2;break;case 5:in.gmxc_present=0;break;
        case 6:in.gmxc.data=NULL;break;case 7:in.sh0.aux.bytes=7;break;case 8:in.mc0.address=in.sh0.address;break;
        case 9:in.acv.address=0;break;case 10:in.cn0_address=0;break;case 11:in.enable_acd=2;break;case 12:in.enable_ifpc=2;break;
        }
        memset(&c,0xa5,sizeof(c));AdrenoHfiStartupConfig before;memcpy(&before,&c,sizeof(c));
        CHECK(ShengGmuBuildStartup(&in,&c)==AdrenoHfiStartupInvalid && !memcmp(&before,&c,sizeof(c)));
    }
    puts("PASS: pinned board OPP/BCM binding, 8 GMxC/ACD/IFPC profiles, 13 all-or-nothing refusals; command-db values are software fixtures only");
}
