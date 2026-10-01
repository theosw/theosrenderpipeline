"""CPU-only execution of pinned provider network selection/initializers.
Allocation, upload and kernel creation are bounded stand-ins; no GPU is used.
"""
from pathlib import Path
import json,struct,sys,hashlib
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('provider',type=Path)
parser.add_argument('sm75_audit',type=Path)
parser.add_argument('sm86_audit',type=Path)
parser.add_argument('output',type=Path)
args=parser.parse_args()
import pefile
from unicorn import Uc,UC_ARCH_X86,UC_MODE_64,UC_HOOK_CODE
from unicorn.x86_const import *
provider=args.provider
raw=provider.read_bytes();assert hashlib.sha256(raw).hexdigest()=='ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82'
p=pefile.PE(data=raw);base=p.OPTIONAL_HEADER.ImageBase;mapped=p.get_memory_mapped_image()
class VM:
 def __init__(self,patched=False):
  self.u=Uc(UC_ARCH_X86,UC_MODE_64);u=self.u
  u.mem_map(base,(len(mapped)+4095)&~4095);u.mem_write(base,mapped)
  self.heap=0x20000000;self.stack=0x30000000;self.hooks=0x40000000
  for a,n in ((self.heap,0x4000000),(self.stack,0x10000),(self.hooks,0x1000)):u.mem_map(a,n)
  self.next=self.heap+0x10000;self.weights=[];self.kernels=[];self.allocations=[]
  self.q(base+0x92420,self.hooks+0x10)
  if patched:
   original=(args.sm75_audit/'network-factory-before.bin').read_bytes()
   selected=(args.sm75_audit/'network-factory-after.bin').read_bytes()
   assert original==mapped[0x4d9f0:0x4dd71] and len(selected)==len(original)
   u.mem_write(base+0x4d9f0,selected)
  u.hook_add(UC_HOOK_CODE,self.hook)
 def q(self,a,v):self.u.mem_write(a,struct.pack('<Q',v))
 def d(self,a,v):self.u.mem_write(a,struct.pack('<I',v))
 def getq(self,a):return struct.unpack('<Q',self.u.mem_read(a,8))[0]
 def alloc(self,n):
  a=self.next;self.next+=(n+15)&~15;assert self.next<self.heap+0x4000000;return a
 def ret(self,v=0):
  u=self.u;sp=u.reg_read(UC_X86_REG_RSP);dest=self.getq(sp)
  u.reg_write(UC_X86_REG_RAX,v);u.reg_write(UC_X86_REG_RSP,sp+8);u.reg_write(UC_X86_REG_RIP,dest)
 def hook(self,u,a,size,_):
  reg=u.reg_read
  if a==self.hooks+0x10:u.reg_write(UC_X86_REG_RIP,reg(UC_X86_REG_RAX))
  elif a==base+0x69f84:self.ret(self.alloc(reg(UC_X86_REG_RCX)))
  elif a in (base+0x67ec0,base+0x6a700):self.ret()
  elif a==base+0x68670:
   n=reg(UC_X86_REG_R8);out=reg(UC_X86_REG_RDX);buf=self.alloc(n)
   self.q(out+16,buf);self.allocations.append(n);self.ret(out)
  elif a==base+0x68cf0:
   src=reg(UC_X86_REG_R8);n=reg(UC_X86_REG_R9)
   self.weights.append({'rva':src-base,'bytes':n,'sha256':hashlib.sha256(bytes(u.mem_read(src,n))).hexdigest()});self.ret(1)
  elif a==base+0x68c60:
   blob=reg(UC_X86_REG_RDX);n=reg(UC_X86_REG_R8);name=reg(UC_X86_REG_R9);sp=reg(UC_X86_REG_RSP)
   self.kernels.append({'rva':blob-base,'bytes':n,'magic':bytes(u.mem_read(blob,4)).hex(),'name':bytes(u.mem_read(name,128)).split(b'\0')[0].decode(),'launch':list(struct.unpack('<4Q',u.mem_read(sp+0x28,32)))})
   self.ret(self.alloc(0x80))
 def run(self,rva,rcx=0,stop=None):
  u=self.u;sp=self.stack+0xf000-8;self.q(sp,self.hooks+0x80)
  u.reg_write(UC_X86_REG_RSP,sp);u.reg_write(UC_X86_REG_RCX,rcx)
  u.emu_start(base+rva,base+stop if stop else self.hooks+0x80,timeout=2000000,count=100000)
  assert u.reg_read(UC_X86_REG_RIP)==(base+stop if stop else self.hooks+0x80),hex(u.reg_read(UC_X86_REG_RIP))
  return u.reg_read(UC_X86_REG_RAX)
def select(arch,patched,second=False):
 v=VM(patched);ctx=v.heap;generic=ctx+0x1000;vt=ctx+0x2000
 v.q(ctx+0x28,generic);v.q(generic,vt);v.q(vt+0x40,base+0x1b6c0);v.d(generic+0x368,arch)
 v.u.reg_write(UC_X86_REG_RBP,ctx)
 v.run(0x4db78 if second else 0x4da37,ctx,0x4dbf3 if second else 0x4dabb)
 obj=v.u.reg_read(UC_X86_REG_RSI if second else UC_X86_REG_RDI)
 return v.getq(obj)-base
def init(ctor,size):
 v=VM();obj=v.heap;v.run(ctor,obj);vt=v.getq(obj);initial=bytes(v.u.mem_read(obj+8,size-8))
 entries=struct.unpack('<12Q',v.u.mem_read(vt,96));fn=entries[4]-base;v.run(fn,obj)
 return {'ctor':ctor,'vtable':vt-base,'init':fn,'initialSHA256':hashlib.sha256(initial).hexdigest(),'vtableEntries':[x-base for x in entries], 'weights':v.weights,'allocations':v.allocations,'kernels':v.kernels}
rows=[]
for arch in (0x160,0x170,0x190,0x1b0):
 for patched in (False,True):
  rows.append({'architecture':hex(arch),'patched':patched,'dl1':hex(select(arch,patched)),'dl2':hex(select(arch,patched,True))})
assert rows[0]['dl1']=='0x64aa48' and rows[0]['dl2']=='0x64ad10', 'old Turing path'
assert rows[1]['dl1']=='0x64abc8' and rows[1]['dl2']=='0x64b008', 'corrected PTX path'
assert (args.sm86_audit/'network-factory-before.bin').read_bytes()==(args.sm86_audit/'network-factory-after.bin').read_bytes()==mapped[0x4d9f0:0x4dd71], 'Ampere selection changed'
print('PASS production-patched Turing factory selects both PTX networks; Ampere factory unchanged')
networks=[]
for constructors,size in (((0x43a50,0x44f30,0x46410),0x1488),((0x478f0,0x49810,0x4b730),0x2060)):
 group=[init(c,size) for c in constructors]
 first=group[0]
 for other in group[1:]:
  assert first['initialSHA256']==other['initialSHA256'],'constructor fields changed'
  assert first['weights']==other['weights'],'weight data changed'
  assert first['allocations']==other['allocations'],'allocations changed'
  assert [x for i,x in enumerate(first['vtableEntries']) if i not in (1,4)]==[x for i,x in enumerate(other['vtableEntries']) if i not in (1,4)],'non-init/dtor dispatch changed'
  assert [(k['name'],k['launch']) for k in first['kernels']]==[(k['name'],k['launch']) for k in other['kernels']],'kernel launch contracts changed'
 assert all(k['magic']=='50ed55ba' for k in group[2]['kernels'])
 print('NETWORK',hex(size),'kernel calls',len(first['kernels']),'weights',len(first['weights']),'same fields, weights, allocations, dispatch and launch contracts')
 networks.extend(group)
# Match every selected kernel to the actual production audit's prepared output.
containers=[]
for section in p.sections:
 if section.Characteristics&0x20000000:continue
 data=section.get_data();i=0
 while (i:=data.find(bytes.fromhex('50ed55ba'),i))>=0:
  size=16+int.from_bytes(data[i+8:i+16],'little')
  containers.append((section.VirtualAddress+i,size));i+=size
assert len(containers)==70
for network in (networks[2],networks[5]):
 for kernel in network['kernels']:
  index=containers.index((kernel['rva'],kernel['bytes']))
  fatbin=(args.sm75_audit/(str(index)+'.fatbin')).read_bytes()
  ptx=(args.sm75_audit/(str(index)+'.ptx')).read_text()
  assert len(fatbin)==kernel['bytes'] and '.target sm_75' in ptx
  assert int.from_bytes(fatbin[:4],'little')==0xba55ed50
  kernel['preparedProgram']=index
args.output.write_text(json.dumps({'selection':rows,'networks':networks},indent=2))
print('PASS CPU-only network selection and initializer contract comparison')
