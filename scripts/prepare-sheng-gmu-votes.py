#!/usr/bin/env python3
"""Derive exact enabled board OPPs/BCM policy; no hardware inputs invented."""
import hashlib,json,re,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def prepare():
 subprocess.run([sys.executable,ROOT/'kits/uke/audit-adreno-platform.py'],check=True,timeout=30)
 pins=json.loads((ROOT/'scripts/data/adreno-inputs.json').read_text());dtb=ROOT/'build/adreno-firmware/sheng.dtb'
 def prop(node,name,kind='x'):
  return subprocess.check_output(['fdtget','-t',kind,dtb,node,name],text=True).split()
 def cells(node,name):return [int(v,16) for v in prop(node,name)]
 def one(node,name):
  v=cells(node,name)
  if len(v)!=1:raise SystemExit('Expected one cell '+name)
  return v[0]
 records={}
 for kind,node,max_count in [('gpu','/soc/gpu@3d00000',16),('gmu','/soc/gmu@3d6a000',4)]:
  table=node+'/opp-table'
  if cells(node,'operating-points-v2')!=cells(table,'phandle'):raise SystemExit('OPP phandle binding mismatch')
  children=subprocess.check_output(['fdtget','-l',dtb,table],text=True).split();entries=[]
  for child in children:
   path=table+'/'+child;keys=set(subprocess.check_output(['fdtget','-p',dtb,path],text=True).split())
   supported={'opp-hz','opp-level','opp-peak-kBps','qcom,opp-acd-level','phandle','status'}
   if keys-supported:raise SystemExit('Unimplemented OPP policy: '+repr(keys-supported))
   if 'status' in keys and prop(path,'status','s')!=['okay']:continue
   hz=cells(path,'opp-hz')
   if len(hz)!=2:raise SystemExit('Expected 64-bit frequency')
   hz=(hz[0]<<32)|hz[1]
   if not hz or hz%1000 or hz//1000>0xffffffff:raise SystemExit('Unrepresentable OPP frequency')
   level=one(path,'opp-level')
   if not 0<level<=65535:raise SystemExit('Unrepresentable ARC level')
   entry=dict(hz=hz,level=level,kbps=one(path,'opp-peak-kBps') if kind=='gpu' else 0,
     acd_present='qcom,opp-acd-level' in keys,acd=one(path,'qcom,opp-acd-level') if 'qcom,opp-acd-level' in keys else 0)
   if kind=='gpu' and not entry['kbps']:raise SystemExit('Missing active bandwidth')
   entries.append(entry)
  entries.sort(key=lambda e:e['hz'])
  if not entries or len(entries)+1>max_count or len({e['hz'] for e in entries})!=len(entries):raise SystemExit('Invalid OPP count/order')
  if any(entries[i]['level']<entries[i-1]['level'] for i in range(1,len(entries))):raise SystemExit('Nonmonotonic OPP levels')
  records[kind]=[dict(hz=0,level=0,kbps=0,acd_present=False,acd=0)]+entries
 path='drivers/gpu/drm/msm/adreno/a6xx_catalog.c';catalog=ROOT/'.cache/adreno-source'/path
 if sha(catalog)!=pins['linux']['files'][path]['sha256']:raise SystemExit('Pinned catalog mismatch')
 record=re.search(r'\.chip_ids = ADRENO_CHIP_IDS\(0x43050a01\)([\s\S]*?)\.preempt_record_size',catalog.read_text())[1]
 entries=re.findall(r'\{\s*\.name = "([A-Z0-9]+)",([^{}]+)\}',record)
 if [e[0] for e in entries]!=['SH0','MC0','ACV']:raise SystemExit('BCM order mismatch')
 policies=[]
 for name,body in entries:
  if name!='ACV':
   m=re.fullmatch(r'\s*\.buswidth = (\d+)\s*',body)
   if not m:raise SystemExit('Unrecognized proportional BCM policy')
   policies.append(dict(name=name,buswidth=int(m[1]),fixed=0,perfmode=0,perfmode_kbps=0))
  else:
   m=re.fullmatch(r'\s*\.fixed = true,\s*\.perfmode = BIT\((\d+)\),\s*\.perfmode_bw = (\d+),\s*',body)
   if not m or int(m[1])>=14:raise SystemExit('Unrecognized fixed BCM policy')
   policies.append(dict(name=name,buswidth=0,fixed=1,perfmode=1<<int(m[1]),perfmode_kbps=int(m[2])))
 bandwidths=sorted({e['kbps'] for e in records['gpu']})
 out=ROOT/'build/sheng-gmu';out.mkdir(exist_ok=True)
 text='/* Generated from pinned DT/catalog; contains no command-db values. */\n#include <stdint.h>\n'
 text+='#define SHENG_GMU_VOTES_CHIP_ID 0x43050a01u\n'
 text+='typedef struct {uint64_t hz;uint32_t level,kbps,acd_present,acd;} ShengGmuOpp;\n'
 for kind in ['gpu','gmu']:
  text+='#define SHENG_'+kind.upper()+'_OPPS '+str(len(records[kind]))+'u\n'
  text+='static const ShengGmuOpp sheng_'+kind+'_opps[]={\n'
  text+='\n'.join('{'+','.join(str(int(e[k])) for k in ['hz','level','kbps','acd_present','acd'])+'},' for e in records[kind])+'\n};\n'
 text+='#define SHENG_GPU_BANDWIDTHS '+str(len(bandwidths))+'u\nstatic const uint32_t sheng_gpu_bandwidths[]={'+','.join(map(str,bandwidths))+'};\n'
 text+='static const AdrenoHfiBcm sheng_bcm_policy[3]={\n'
 text+='\n'.join('{.buswidth='+str(e['buswidth'])+',.fixed='+str(e['fixed'])+',.perfmode='+str(e['perfmode'])+',.perfmode_kbps='+str(e['perfmode_kbps'])+'},' for e in policies)+'\n};\n'
 header=out/'ShengGmuVotesSource.h';header.write_text(text)
 db_node='/reserved-memory/aop-cmd-db-region@81c60000'
 if prop(db_node,'compatible','s')!=['qcom,cmd-db']:raise SystemExit('Command DB resource binding mismatch')
 db_range=cells(db_node,'reg')
 if len(db_range)!=4:raise SystemExit('Command DB range format differs')
 subprocess.run(['fdtget',dtb,db_node,'no-map'],check=True,capture_output=True)
 db_base=(db_range[0]<<32)|db_range[1];db_bytes=(db_range[2]<<32)|db_range[3]
 if db_bytes<144 or db_bytes>1048576:raise SystemExit('Unsupported command DB reservation size')
 db_header=out/'ShengGmuCommandDbSource.h'
 db_header.write_text('/* Pinned DT reservation, not observed hardware memory. */\n#define SHENG_GMU_COMMAND_DB_BASE UINT64_C('+hex(db_base)+')\n#define SHENG_GMU_COMMAND_DB_BYTES '+str(db_bytes)+'u\n')

 report=dict(linux_revision=pins['linux']['commit'],board_revision=pins['board']['commit'],dtb_sha256=sha(dtb),catalog_sha256=sha(catalog),
  command_db_base=db_base,command_db_bytes=db_bytes,command_db_header_sha256=sha(db_header),opps=records,bandwidths_kbps=bandwidths,bcms=policies,generated_header_sha256=sha(header),actual_command_db_available=False,hardware_ready=False)
 (out/'votes-source-report.json').write_text(json.dumps(report,indent=2)+'\n')
 print('PASS: pinned enabled OPPs, deduplicated bandwidths, ACD and A740 BCM policy; actual command-db remains a required input')
 return out,report
if __name__=='__main__':prepare()
