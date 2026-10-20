/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengGpuSmmu.h"
#include "ShengGpuSmmuSource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t regs[SHENG_GPU_SMMU_BYTES/4];
static struct {unsigned calls,fail_call,after,writes,route_fail,fault_fail,timeout;size_t table_calls,table_fail,table_mutate;unsigned mutation;size_t cb;unsigned slots[2],count,bank;} f;
static ArmSmmu4KIo io;
static ShengGpuSmmuSession session;
static ShengGpuSmmuResult result;
static ArmSmmu4KContextConfig context;
static ArmSmmu4KRange a={0x4000,0x20000000,8192,15,0,2},b={UINT64_C(0xffffff0000004000),0x21000000,8192,7,1,3};
static unsigned char lv[8],uv[8];
static ArmSmmu4KAuditRegion la={&a,1,8,4096,lv,8},ua={&b,1,8,4096,uv,8};
static ArmSmmu4KAuditConfig audit;
static ArmSmmu4KConfig lower={32,40,0x10000000,2},upper={40,40,0x11000000,2};
static ArmSmmu4KResult lr,ur;static uint64_t lt[8*512],ut[8*512];
static int table_read(void *cookie,uint64_t pa,uint64_t *v)
{
    CHECK(cookie==&f && !(pa&7));
    if(++f.table_calls==f.table_fail)return 1;
    if(f.table_calls==f.table_mutate) {
        if(f.mutation==0)regs[(f.cb+0x20)/4]^=4096;
        else if(f.mutation==1)regs[(f.cb+0x30)/4]^=1u<<14;
        else if(f.mutation==2)regs[(f.cb+0x58)/4]|=2;
        else if(f.mutation==3)regs[(0xc00+4*f.slots[0])/4]^=1;
        else if(f.mutation==4)regs[0x24/4]^=1;
        else if(f.mutation==5)regs[f.cb/4]^=1;
        else if(f.mutation==6)regs[(f.cb+0x28)/4]^=4096;
        else if(f.mutation==7)regs[(f.cb+0x30)/4]^=1u<<30;
        else regs[0x28/4]^=1u<<12;
    }
    if(pa>=lower.table_physical && pa-lower.table_physical<sizeof(lt))*v=lt[(pa-lower.table_physical)/8];
    else {CHECK(pa>=upper.table_physical && pa-upper.table_physical<sizeof(ut));*v=ut[(pa-upper.table_physical)/8];}
    return 0;
}
static int event(void){return ++f.calls==f.fail_call;}
static int read32(void *cookie,size_t at,uint32_t *v)
{CHECK(cookie==&f && at%4==0 && at+4<=sizeof(regs));if(event())return 1;*v=(at==f.cb+0x7f4 && f.timeout)?1:regs[at/4];return 0;}
static int read64(void *cookie,size_t at,uint64_t *v)
{CHECK(cookie==&f && at%8==0 && at+8<=sizeof(regs));if(event())return 1;memcpy(v,regs+at/4,8);return 0;}
static int write32(void *cookie,size_t at,uint32_t v)
{
    CHECK(cookie==&f && at%4==0 && at+4<=sizeof(regs));++f.writes;int fail=event();
    if(fail && !f.after)return 1;
    unsigned own=at==0x1000+4*f.bank || at==0x1800+4*f.bank || (at>=f.cb && at<f.cb+4096);
    for(unsigned i=0;i<f.count;++i)if(at==0x800+4*f.slots[i] || at==0xc00+4*f.slots[i])own=1;
    CHECK(own); /* no other bank, slot, global or device registers */
    if(at==f.cb && !v)for(unsigned i=0;i<f.count;++i)CHECK(((regs[(0xc00+4*f.slots[i])/4]>>16)&3)==2);
    if(f.fault_fail && (v&0x30000)==0x20000 && at>=0xc00 && at<0x1000)return 1;
    if(at==f.cb+0x58)regs[at/4]&=~v;else regs[at/4]=v;
    if(f.route_fail && at>=0xc00 && at<0x1000 && !(v&0x30000))return 1;
    return fail;
}
static int write64(void *cookie,size_t at,uint64_t v)
{
    CHECK(cookie==&f && (at==f.cb+0x20 || at==f.cb+0x28));++f.writes;int fail=event();
    if(fail && !f.after)return 1;memcpy(regs+at/4,&v,8);return fail;
}
static int barrier(void *cookie){CHECK(cookie==&f);return event();}
static void reset(unsigned role,unsigned exid)
{
    memset(regs,0,sizeof(regs));memset(&f,0,sizeof(f));session=(ShengGpuSmmuSession){0};
    f.bank=role?2:0;f.count=role?1:2;f.slots[0]=2;f.slots[1]=4;f.cb=0x20000+f.bank*4096;
    io=(ArmSmmu4KIo){&f,sizeof(regs),read32,read64,write32,write64,barrier};
    regs[0]=0x436|(exid<<3);regs[0x20/4]=(1u<<30)|(1u<<27)|(4u<<9)|8|(exid<<8);
    regs[0x24/4]=0x40000010;regs[0x28/4]=0x1433;
    for(unsigned i=0;i<8;++i)regs[(0xc00+4*i)/4]=0x20000;
    regs[0x800/4]=(exid?0:1u<<31)|9;regs[0xc00/4]=(exid?1u<<10:0)|3;
    audit=(ArmSmmu4KAuditConfig){{&f,table_read},role?&la:NULL,role?NULL:&ua};
    context=(ArmSmmu4KContextConfig){.asid=f.bank+1,.coherent_walk=1,.continue_after_fault=1};
    if(role){context.lower=&lower;context.lower_tables=&lr;}else{context.upper=&upper;context.upper_tables=&ur;}
}
static ArmSmmu4KStatus start(unsigned role)
{return ShengGpuSmmuStart(&io,SHENG_GPU_SMMU_BASE,(ShengGpuSmmuRole)role,f.bank,f.slots,f.count,&context,&audit,4,&session,&result);}
static _Alignas(4096) uint64_t live_tables[8*512];
static ArmSmmu4KLiveArena live_arena;
static ArmSmmu4KLiveState live_state;
static void reset_live(unsigned role,unsigned exid)
{
    reset(role,exid);live_state=(ArmSmmu4KLiveState){0};
    live_arena=(ArmSmmu4KLiveArena){{role?32:40,40,0x12000000,8},
        role?UINT64_C(0x1ff000):UINT64_C(0xffffff00001ff000),8192,!role,live_tables,8};
    CHECK(!ArmSmmu4KLivePrepare(&live_arena,&live_state));
    context.lower=role?&live_arena.config:NULL;context.lower_tables=role?&live_state.tables:NULL;
    context.upper=role?NULL:&live_arena.config;context.upper_tables=role?NULL:&live_state.tables;
}
static ArmSmmu4KStatus start_live(unsigned role)
{return ShengGpuSmmuStartLive(&io,SHENG_GPU_SMMU_BASE,(ShengGpuSmmuRole)role,f.bank,f.slots,f.count,
    &context,&live_arena,&live_state,4,&session,&result);}
static int publish_live(void *cookie,volatile uint64_t *page,size_t bytes)
{CHECK(cookie==&f && bytes==4096 && page>=live_tables && page<live_tables+8*512 && !((uintptr_t)page&4095));return barrier(cookie);}
static void live_lifecycle(void)
{
    unsigned starts=0,stops=0,mutations=0;
    for(unsigned role=0;role<2;++role)for(unsigned exid=0;exid<2;++exid) {
        reset_live(role,exid);CHECK(!start_live(role));
        unsigned normal=f.calls;CHECK(session.ready && result.live.complete && !live_state.tables.leaf_pages);
        CHECK(result.context.enabled && result.context.readback_valid && !session.poisoned);
        unsigned writes=f.writes;
        CHECK(!ShengGpuSmmuObserveLive(&io,SHENG_GPU_SMMU_BASE,&session,NULL,0,&result));
        CHECK(f.writes==writes && result.live.complete && result.must_retain_resources);
        CHECK(ShengGpuSmmuObserve(&io,SHENG_GPU_SMMU_BASE,&session,&result)==ArmSmmu4KArgument);
        ArmSmmu4KRange range={live_arena.iova,0x25000000,8192,7,1,3};
        ArmSmmu4KLiveVisibility visibility={&f,publish_live};ArmSmmu4KLiveResult changed;
        CHECK(!ArmSmmu4KLiveUpdate(&live_arena,&live_state,&io,f.bank,&context,&visibility,NULL,0,&range,1,4,&changed));
        CHECK(changed.complete && live_state.tables.leaf_pages==2);
        CHECK(!ShengGpuSmmuObserveLive(&io,SHENG_GPU_SMMU_BASE,&session,&range,1,&result));
        CHECK(ShengGpuSmmuObserveLive(&io,SHENG_GPU_SMMU_BASE,&session,NULL,0,&result)==ArmSmmu4KArgument);
        f.calls=0;
        CHECK(!ShengGpuSmmuStop(&io,SHENG_GPU_SMMU_BASE,4,&session,&result));
        unsigned stop_calls=f.calls;CHECK(!session.ready && !session.poisoned && !regs[f.cb/4]);
        for(unsigned at=1;at<=normal;++at)for(unsigned after=0;after<2;++after) {
            reset_live(role,exid);f.fail_call=at;f.after=after;
            CHECK(start_live(role)==ArmSmmu4KIoError && !session.ready && result.must_retain_resources);
            if(session.attempted)CHECK(session.poisoned && live_state.poisoned);else CHECK(!f.writes);
            ++starts;
        }
        for(unsigned at=1;at<=stop_calls;++at)for(unsigned after=0;after<2;++after) {
            reset_live(role,exid);CHECK(!start_live(role));f.calls=0;f.fail_call=at;f.after=after;
            CHECK(ShengGpuSmmuStop(&io,SHENG_GPU_SMMU_BASE,4,&session,&result)==ArmSmmu4KIoError);
            CHECK(session.poisoned && live_state.poisoned && !session.ready && result.must_retain_resources);++stops;
        }
        reset_live(role,exid);size_t slots=live_state.tables.table_pages*512;
        for(size_t slot=0;slot<slots;++slot) {
            reset_live(role,exid);live_tables[slot]^=1;
            CHECK(start_live(role)==ArmSmmu4KStateMismatch && !f.calls && !f.writes && !session.attempted);++mutations;
        }
        reset_live(role,exid);CHECK(!start_live(role));
        writes=f.writes;live_tables[0]^=1;
        CHECK(ShengGpuSmmuObserveLive(&io,SHENG_GPU_SMMU_BASE,&session,NULL,0,&result)==ArmSmmu4KStateMismatch);
        CHECK(f.writes==writes && result.must_retain_resources);
        reset_live(role,exid);f.route_fail=f.fault_fail=1;
        CHECK(start_live(role)==ArmSmmu4KIoError && session.poisoned && live_state.poisoned && (regs[f.cb/4]&1));
        reset_live(role,exid);f.timeout=1;
        CHECK(start_live(role)==ArmSmmu4KTimeout && session.poisoned && live_state.poisoned);
        reset_live(role,exid);live_arena.upper_region=role;
        CHECK(start_live(role)==ArmSmmu4KArgument && !f.calls && !f.writes);
        reset_live(role,exid);unsigned bank=f.bank;f.bank=role?0:1;
        CHECK(start_live(role)==ArmSmmu4KArgument && !f.calls && !f.writes);f.bank=bank;
    }
    printf("PASS: live arenas, %u live-start and %u live-stop I/O failures, %u reserved-graph mutations; actual context/stream observations, mapped resources and fault-before-disable\n",starts,stops,mutations);
}
int main(void)
{
    /* Source-required GMU firmware permissions: RW+execute+privileged. Native
     * 4K leaves only; these are explicitly synthetic physical allocations. */
    CHECK(!ArmSmmu4KBuild(&lower,&a,1,lt,8,&lr));CHECK(!ArmSmmu4KBuildUpper(&upper,&b,1,ut,8,&ur));
    unsigned failures=0,stop_failures=0,table_failures=0,mutation_refusals=0;
    for(unsigned role=0;role<2;++role)for(unsigned exid=0;exid<2;++exid) {
        reset(role,exid);CHECK(!start(role) && session.ready && result.must_retain_resources);
        unsigned normal=f.calls;size_t table_reads=f.table_calls;
        CHECK(result.audit.complete && result.audit.before.readback_valid && result.audit.after.readback_valid);
        CHECK(session.streams.bank==f.bank && session.streams.count==f.count);
        CHECK(session.streams.streams[0].sid==(role?5:0));
        if(!role)CHECK(session.streams.streams[1].sid==1);
        unsigned writes=f.writes;
        CHECK(!ShengGpuSmmuObserve(&io,SHENG_GPU_SMMU_BASE,&session,&result) && f.writes==writes);
        CHECK(start(role)==ArmSmmu4KBusy && f.writes==writes);
        f.calls=0;
        CHECK(!ShengGpuSmmuStop(&io,SHENG_GPU_SMMU_BASE,4,&session,&result) && !session.ready && !session.poisoned && result.must_retain_resources && regs[f.cb/4]==0);
        unsigned stop_calls=f.calls;
        for(unsigned at=1;at<=normal;++at)for(unsigned after=0;after<2;++after) {
            reset(role,exid);f.fail_call=at;f.after=after;
            CHECK(start(role)==ArmSmmu4KIoError && !session.ready && result.must_retain_resources);
            if(session.attempted)CHECK(session.poisoned);else CHECK(!f.writes);
            ++failures;
        }
        for(unsigned at=1;at<=stop_calls;++at)for(unsigned after=0;after<2;++after) {
            reset(role,exid);CHECK(!start(role));f.calls=0;f.fail_call=at;f.after=after;
            CHECK(ShengGpuSmmuStop(&io,SHENG_GPU_SMMU_BASE,4,&session,&result)==ArmSmmu4KIoError && session.poisoned && !session.ready && result.must_retain_resources);
            ++stop_failures;
        }
        for(size_t at=1;at<=table_reads;++at) {
            reset(role,exid);f.table_fail=at;
            CHECK(start(role)==ArmSmmu4KIoError && !session.ready && result.must_retain_resources);
            if(session.attempted)CHECK(session.poisoned && !regs[f.cb/4]);else CHECK(!f.writes);
            ++table_failures;
        }
        for(unsigned mutation=0;mutation<9;++mutation) {
            reset(role,exid);f.table_mutate=table_reads/2+1;f.mutation=mutation;
            ArmSmmu4KStatus status=start(role);
            CHECK((status==ArmSmmu4KStateMismatch || status==ArmSmmu4KFault || status==ArmSmmu4KUnsupported) && !session.ready && session.poisoned && result.must_retain_resources);
            ++mutation_refusals;
        }
        reset(role,exid);uint64_t *table=role?lt:ut;uint64_t old=table[0];table[0]^=1;
        CHECK(start(role)==ArmSmmu4KStateMismatch && !f.writes && !session.attempted);table[0]=old;
        reset(role,exid);f.route_fail=f.fault_fail=1;
        CHECK(start(role)==ArmSmmu4KIoError && session.poisoned && (regs[f.cb/4]&1) && result.cleanup_status!=ArmSmmu4KSuccess);
        reset(role,exid);f.timeout=1;
        CHECK(start(role)==ArmSmmu4KTimeout && session.poisoned && !session.ready && result.must_retain_resources);
        reset(role,exid);regs[0xc00/4]=(exid?1u<<10:0)|f.bank;
        CHECK(start(role)==ArmSmmu4KBusy && !f.writes && !session.attempted); /* foreign SID on bank */
        reset(role,exid);CHECK(ShengGpuSmmuStart(&io,SHENG_GPU_SMMU_BASE+4096,(ShengGpuSmmuRole)role,f.bank,f.slots,f.count,&context,&audit,4,&session,&result)==ArmSmmu4KArgument && !f.calls);
        context.continue_after_fault=0;CHECK(start(role)==ArmSmmu4KArgument && !f.calls);
    }
    reset(0,1);context.lower=&lower;context.lower_tables=&lr;audit.lower=&la;
    CHECK(!start(0) && result.audit.lower.complete && result.audit.upper.complete);
    CHECK(f.table_calls==2*(lr.table_pages+ur.table_pages)*512);
    CHECK(!ShengGpuSmmuStop(&io,SHENG_GPU_SMMU_BASE,4,&session,&result));
    printf("PASS: %u physical-read failures, %u during-walk context/stream mutations refused\n",table_failures,mutation_refusals);
    printf("PASS: GPU/GMU lower/upper native4K lifecycle, legacy/EXIDS, %u startup and %u stop I/O failures, no bank disable before stream fault, retained resources; software register peers only\n",failures,stop_failures);
    live_lifecycle();
}
