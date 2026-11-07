"""Own shader build driver. Only compiles allowlisted, copied sources."""
import argparse,json,subprocess
from pathlib import Path
import os
root=Path(__file__).resolve().parents[1];bulk=Path(os.environ['TRP_VKNR_BUILD'])
p=argparse.ArgumentParser();p.add_argument('--backend',choices=['portable','rdna3'],default='portable');p.add_argument('--only');p.add_argument('--source',type=Path);p.add_argument('--dest',type=Path);p.add_argument('--define',action='append',default=[]);p.add_argument('--shared48',action='store_true');a=p.parse_args()
table=json.loads((root/'vendor/mauri/linux/shaders/rdna3/pipelines.json').read_text())
s=a.source or bulk/'engine-src/shaders'/a.backend
dest=a.dest or bulk/'engine-package'/a.backend/'dlssnr-amd/shaders';dest.mkdir(parents=True,exist_ok=True)
tools=Path(os.environ['TRP_VKNR_TOOLS'])
jobs=[(name,e,'g_'+name) for name,e in table['pipelines'].items()]
if a.backend=='portable':
 for name,e,relative in list(jobs):
  if name in ['fswin128','fswinds128','fswindsp128','fswinfusedup128']:
   jobs.append((name+'shared48',dict(source=e['source'],defines=e['defines']+['NR_PORT_SHARED_XY=1']),relative+'.spv.shared48'))
# Alternative register shapes retain the same output tile and are property-selected.
if a.backend=='portable':
 for base,wn in [('gemmprojw',4),('gemmvact',8)]:
  e=table['pipelines'][base]
  defines=[d for d in e['defines'] if not d.startswith(('NR_WM=','NR_WN=','NR_MFRAG=','NR_NFRAG='))]
  defines+=['NR_WM=1','NR_WN='+str(wn),'NR_MFRAG=4','NR_NFRAG=2']
  jobs.append((base+'_deep_m4n2',dict(source=e['source'],defines=defines),'g_'+base+'_deep_m4n2'))
for name,v in table['variants'].items():
 base=table['pipelines'][v['base']];jobs.append((name,dict(source=base['source'],defines=base['defines']+v['add']),'temporal/'+name))
for name in ['runtime_alpha','runtime_depth','runtime_reuse_store','runtime_reuse_motion','runtime_reuse_warp','runtime_reuse_stat','motion_luma','motion_estimate']:
 jobs.append((name,dict(source='../passes/'+name+'.comp',defines=[]),('temporal/' if name.startswith('motion_') else 'runtime/')+name))
for name,e,relative in jobs:
 if a.only and name not in a.only.split(','):continue
 defines=[d for d in e['defines'] if not (a.backend=='portable' and d.startswith('NR_UPS_ALIAS='))]
 if a.backend=='portable':
  if name=='vitattn':defines+=['NR_PORT_SUBGROUPS=1']
  if name.startswith('fswinfusedup'):defines+=['NR_DEEP_DECODER=1']
  if name in ('gemmprojw','gemmvact','gemmvqkvnorms','gemmprojw_deep_m4n2','gemmvact_deep_m4n2'):
   defines+=['NR_PORTABLE_GEMM_CORE=1']
  if name.startswith('fswin') and not name.startswith('fswinp') and 'image' not in name:
   values=dict((d.split('=',1)+['1'])[:2] for d in defines)
   count=int(values.get('NR_HEADS','1')) if int(values.get('NR_HWAVES','0')) else int(values.get('NR_FWAVES','2'))
   count*=int(values.get('NR_PBATCH','1'));assert 1<=count<=8
   defines += [f'NR_PORT_SUBGROUPS={count}','NR_PORT_WINDOW_ACOL=1']
   if not name.startswith('fswinfusedup') and int(values.get('NR_C','32'))>=64:
    defines+=['NR_WINDOW_NATIVE_EXPAND=1','NR_WINDOW_NATIVE_QKV=1','NR_WINDOW_NATIVE_RESIDUAL=3','NR_WINDOW_NATIVE_MIDDLE=1']
   if not name.startswith('fswinfusedup') and int(values.get('NR_C','32'))==32:
    defines+=['NR_WINDOW_NATIVE_QKV32=1']
 defines+=a.define
 output=dest/(relative+'.spv');output.parent.mkdir(parents=True,exist_ok=True)
 cmd=[str(tools/'glslang/glslang.exe'),'-V','--target-env','vulkan1.3','-I'+str(s/'include'),'-I'+str(s.parent/'passes/include')]+['-D'+d for d in defines]+[str(s/e['source']),'-o',str(output)]
 r=subprocess.run(cmd,capture_output=True,text=True)
 (dest/(name+'.build.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stdout,r.stderr);raise SystemExit(r.returncode)
 if a.backend=='portable':
  original=output.with_suffix('.unoptimized.spv');original.write_bytes(output.read_bytes())
  r=subprocess.run([str(tools/'spirv-tools/spirv-opt.exe'),'-O','--target-env=vulkan1.3',str(original),'-o',str(output)],capture_output=True,text=True)
  if r.returncode:print(r.stdout,r.stderr);raise SystemExit(r.returncode)
 r=subprocess.run([str(tools/'spirv-tools/spirv-val.exe'),'--target-env','vulkan1.3',str(output)],capture_output=True,text=True)
 if r.returncode:print(r.stdout,r.stderr);raise SystemExit(r.returncode)
 print(name,'compiled and SPIR-V validated',flush=True)
if not a.only:
 for name,value in table['markers'].items():
  text='\n'.join(value) if isinstance(value,list) else value
  if a.backend=='portable':text=text.replace('kswap 1','kswap 0')
  (dest/name).write_text(text+'\n')
  if name=='shader-constants.txt':
   (dest/'temporal'/name).write_text(text+'\n');(dest/'runtime'/name).write_text(text+'\n')
