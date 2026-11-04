/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UkeGpuSystemCache.h"
#include <cassert>
#include <cstdio>
#include <unordered_map>
#include <vector>
using namespace sm7675_llcc;
struct Write {unsigned region;uint32_t at,value;};
struct Model {
 unsigned banks=4,ops=0,fail=0,delays=0;bool stuck=false,bad_route=false;
 std::unordered_map<uint64_t,uint32_t> regs;
 std::vector<Write> writes;
 static uint64_t key(unsigned r,uint32_t at){return (uint64_t(r)<<32)|at;}
 uint32_t &reg(unsigned region,uint32_t at){return regs[key(region,at)];}
 Model(unsigned count=4):banks(count) {
  reg(0,CommonStatus)=count<<28;
  reg(2,Route0)=0x80000040;reg(2,Route1)=0xc0000000;
  for(unsigned b=0;b<count && b<4;++b) {
   for(unsigned s:{9u,11u})reg(0,b*BankStride+s*0x1000+4)=2;
   for(uint32_t at:PolicyRegs)reg(0,b*BankStride+at)=0xa5000012;
  }
 }
 Sm7675LlccIo io() {
  return {this,[](void *c,unsigned r,uint32_t at,uint32_t *v){
   auto &m=*static_cast<Model *>(c);if(++m.ops==m.fail)return false;
   *v=m.reg(r,at);if(m.bad_route && r==2 && !m.writes.empty() && at==Route0)*v^=1;
   return true;
  },[](void *c,unsigned r,uint32_t at,uint32_t v){
   auto &m=*static_cast<Model *>(c);if(++m.ops==m.fail)return false;
   m.writes.push_back({r,at,v});m.reg(r,at)=v;
   if(r==1)for(unsigned b=0;b<m.banks;++b) {
    m.reg(0,b*BankStride+at)=v;
    for(unsigned id:{9u,11u}) {
     auto &status=m.reg(0,b*BankStride+id*0x1000+4);
     if(at==id*0x1000 && v==2)status=m.stuck?0x12:0x11;
     if(at==id*0x1000+8 && v==1)status&=~16u;
    }
   }
   return true;
  },[](void *){},[](void *c,unsigned us){static_cast<Model *>(c)->delays+=us;}};
 }
 void existing(bool active=true) {
  for(unsigned b=0;b<banks;++b) {
   for(auto s:{UkeLlccPolicy.gpu,UkeLlccPolicy.htw}) {
    reg(0,b*BankStride+Attr0+s.id*8)=0;
    reg(0,b*BankStride+Attr1+s.id*8)=((s.kib*16/banks)<<16)|16u|(s.fixed<<3);
    reg(0,b*BankStride+Attr2+s.id*4)=0x3fff;
    reg(0,b*BankStride+s.id*0x1000+4)=active?1:2;
   }
   for(unsigned k=0;k<11;++k) {
    auto &v=reg(0,b*BankStride+PolicyRegs[k]);v&=~((1u<<9)|(1u<<11));if(k<2)v|=1u<<9;
   }
  }
 }
};
static Sm7675LlccStatus run(Model &m,Sm7675LlccReport &r,bool apply=true) {
 return configure(m.io(),UkeLlccPolicy,apply,r);
}
static void untouched_clients(const Model &before,Model &after) {
 const uint32_t mask=(1u<<9)|(1u<<11);
 for(unsigned b=0;b<after.banks;++b)for(uint32_t at:PolicyRegs) {
  const auto it=before.regs.find(Model::key(0,b*BankStride+at));assert(it!=before.regs.end());
  assert((it->second&~mask)==(after.reg(0,b*BankStride+at)&~mask));
 }
 for(const auto &w:after.writes) {
  if(w.region==2){assert(w.at==Route0 || w.at==Route1);continue;}
  assert(w.region==1);
  bool legal=false;for(uint32_t at:PolicyRegs)if(w.at==at)legal=true;
  for(unsigned id:{9u,11u})if(w.at==Attr0+id*8 || w.at==Attr1+id*8 || w.at==Attr2+id*4 ||
      w.at==id*0x1000 || w.at==id*0x1000+8)legal=true;
  assert(legal);
 }
}
int main() {
 static_assert(UkeLlccBase==0x25000000 && UkeLlccBytes==0x800000);
 static_assert(UkeLlccBroadcast==0x25800000 && UkeLlccBroadcastBytes==0x200000);
 static_assert(UkeLlccDefaultMode==2 && UkeLlccPolicy.gpu.id==9 && UkeLlccPolicy.gpu.kib==3200);
 static_assert(UkeLlccPolicy.htw.id==11 && UkeLlccPolicy.htw.kib==256);
 for(unsigned n:{1u,2u,4u}) {
  Model m(n),before=m;Sm7675LlccReport r;
  assert(run(m,r)==LlccOk && r.step==5 && r.banks==n && r.initialized==3 && r.activated==3);
  assert(r.active_before[0]==0 && r.active_after[0]==(1u<<n)-1);
  assert(m.reg(2,Route0)==0x80002540 && m.reg(2,Route1)==0xd294a529);
  untouched_clients(before,m);
  // GPU reset clears GBIF state; the LLCC setup is idempotent and re-routes it.
  m.writes.clear();m.reg(2,Route0)=0;m.reg(2,Route1)=0;
  assert(run(m,r)==LlccOk && r.initialized==0 && r.activated==0);
  for(const auto &w:m.writes)assert(w.region==2);
 }
 for(bool active:{false,true}) {
  Model m; m.existing(active);Model before=m;Sm7675LlccReport r;
  assert(run(m,r)==LlccOk && r.initialized==0 && r.activated==(active?0u:3u));
  untouched_clients(before,m);
 }
 {Model m;Sm7675LlccReport r;assert(run(m,r,false)==LlccObserved && m.writes.empty());}
 {Model m;m.existing();Sm7675LlccReport r;assert(run(m,r,false)==LlccObserved && m.writes.empty());}
 for(unsigned n:{0u,3u,5u,15u}) {Model m(n);Sm7675LlccReport r;assert(run(m,r)==LlccBankCount && m.writes.empty());}
 for(unsigned scenario=0;scenario<7;++scenario) {
  Model m;Sm7675LlccReport r;
  switch(scenario) {
   case 0:m.existing();m.reg(0,Attr1+9*8)^=1u<<16;break; // active capacity mismatch
   case 1:m.reg(0,Attr1+9*8)=1;break; // unknown inactive policy
   case 2:m.reg(0,BankStride+PolicyRegs[0])^=1;break; // per-bank client difference
   case 3:m.existing();m.reg(0,9*0x1000+4)=3;break; // transition state
   case 4:m.reg(0,9*0x1000+4)=1;break; // active blank slice
   case 5:m.existing();for(unsigned b=0;b<4;++b)m.reg(0,b*BankStride+PolicyRegs[0])&=~(1u<<9);break;
   case 6:m.reg(0,BankStride+Attr2+11*4)=0x1;break;
  }
  assert(run(m,r)==LlccConflict && m.writes.empty() && r.failed_offset);
 }
 {Model m;m.stuck=true;Sm7675LlccReport r;assert(run(m,r)==LlccTimeout && r.polls==100 && m.delays==100);
  assert(m.reg(2,Route0)==0x80000040 && m.reg(2,Route1)==0xc0000000);}
 {Model m;m.bad_route=true;Sm7675LlccReport r;assert(run(m,r)==LlccReadback);
  assert(m.reg(2,Route0)==0x80000040 && m.reg(2,Route1)==0xc0000000);}
 Model successful;Sm7675LlccReport r;assert(run(successful,r)==LlccOk);
 const unsigned operations=successful.ops;
 for(unsigned fault=1;fault<=operations;++fault) {
  Model m;m.fail=fault;Sm7675LlccReport e;assert(run(m,e)!=LlccOk);
  assert(m.reg(2,Route0)==0x80000040 && m.reg(2,Route1)==0xc0000000);
 }
 for(uint32_t seed=1;seed<=1000;++seed) {
  Model m;
  for(uint32_t at:PolicyRegs)for(unsigned b=0;b<4;++b)m.reg(0,b*BankStride+at)=seed*0x1f123bb5;
  Model before=m;assert(run(m,r)==LlccOk);untouched_clients(before,m);
 }
 std::printf("PASS: LLCC cold/existing/reset/read-only, all %u I/O faults, conflicts, timeout, route restore and 1000 preserved-client policies\n",operations);
}
