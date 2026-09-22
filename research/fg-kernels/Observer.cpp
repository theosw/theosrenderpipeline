#include "Observer.h"
#include <atomic>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <fstream>
#include <format>
#include <cstring>
#include <stdexcept>

namespace {
// Experimental NVIDIA NVAPI ABI, pinned to NVIDIA/nvapi 87dca625.
// We retain the original call, arguments and batching; no substitute kernels.
struct Dim{unsigned x,y,z;};
struct Launch{void* function;Dim grid,block;unsigned shared;const void* params;unsigned size;};
struct LaunchEx{void* function;Dim grid,block;unsigned shared;const void* params;unsigned size;void** kernelParams;};
static_assert(sizeof(Launch)==56 && offsetof(Launch,params)==40 && sizeof(LaunchEx)==64);
struct CubinParams{size_t inSize,outSize;ID3D12Device* device;const void* cubin;unsigned size,bx,by,bz,shared;const char* name;unsigned flags;void* shader;};
static_assert(sizeof(CubinParams)==80 && offsetof(CubinParams,name)==56);
using Query=void*(__cdecl*)(unsigned);
using Resolver=FARPROC(WINAPI*)(HMODULE,LPCSTR);
using Create= int(__cdecl*)(ID3D12Device*,const void*,unsigned,unsigned,unsigned,unsigned,unsigned,const char*,void**);
using CreateName= int(__cdecl*)(ID3D12Device*,const void*,unsigned,unsigned,unsigned,unsigned,const char*,void**);
using CreateBasic= int(__cdecl*)(ID3D12Device*,const void*,unsigned,unsigned,unsigned,unsigned,void**);
using CreateV2=int(__cdecl*)(CubinParams*);
using CreateFunction=int(__cdecl*)(ID3D12Device*,void*,const char*,void**);
using Run=int(__cdecl*)(ID3D12GraphicsCommandList*,void*,unsigned,unsigned,unsigned,const void*,unsigned);
using RunChain=int(__cdecl*)(ID3D12GraphicsCommandList*,const Launch*,unsigned);
using RunChainEx=int(__cdecl*)(ID3D12GraphicsCommandList*,const LaunchEx*,unsigned);
using Destroy=int(__cdecl*)(ID3D12Device*,void*);
struct Kernel{std::string name;Dim block;unsigned shared;uint64_t generation;};
struct Record{unsigned group,generated,call,index;Kernel kernel;Dim grid;std::vector<unsigned char> params;unsigned start,end;};
struct State {
    Query query{};Resolver resolve{};HMODULE nvapi{};
    Create create{};CreateName createName{};CreateBasic createBasic{};CreateV2 createV2{};CreateFunction createFunction{};
    Run run{};RunChain chain{};RunChainEx chainEx{};Destroy destroy{},destroyFunction{};
    std::mutex mutex;std::map<void*,Kernel> names;std::vector<Record> records;
    std::ofstream trace,interfaces,lifetimes;
    ID3D12GraphicsCommandList* list{};ID3D12QueryHeap* queries{};
    DWORD thread{};unsigned group{},generated{},call{},next{};uint64_t generation{};
    bool profile{};std::atomic<bool> active{},failed{};
};
State* s{};
void Remember(void* handle,const char* name,Dim block,unsigned shared) noexcept {
    try{std::lock_guard lock(s->mutex);auto gen=++s->generation;
        s->names[handle]={name?name:"<unnamed>",block,shared,gen};
        s->lifetimes<<std::format("create\t{}\t{}\t{}\n",handle,gen,name?name:"<unnamed>");
    }catch(...){s->failed=true;}
}
int __cdecl CEx(ID3D12Device* d,const void* c,unsigned z,unsigned x,unsigned y,unsigned b,unsigned sh,const char* n,void** o){
    auto r=s->create(d,c,z,x,y,b,sh,n,o);if(r==0 && o)Remember(*o,n,{x,y,b},sh);return r;
}
int __cdecl CName(ID3D12Device* d,const void* c,unsigned z,unsigned x,unsigned y,unsigned b,const char* n,void** o){
    auto r=s->createName(d,c,z,x,y,b,n,o);if(r==0 && o)Remember(*o,n,{x,y,b},0);return r;
}
int __cdecl CBasic(ID3D12Device* d,const void* c,unsigned z,unsigned x,unsigned y,unsigned b,void** o){
    auto r=s->createBasic(d,c,z,x,y,b,o);if(r==0 && o)Remember(*o,nullptr,{x,y,b},0);return r;
}
int __cdecl CV2(CubinParams* p){auto r=s->createV2(p);if(r==0 && p)Remember(p->shader,p->name,{p->bx,p->by,p->bz},p->shared);return r;}
int __cdecl CFunction(ID3D12Device* d,void* m,const char* n,void** o){auto r=s->createFunction(d,m,n,o);if(r==0 && o)Remember(*o,n,{},0);return r;}
void Forget(void* handle) noexcept {try{std::lock_guard lock(s->mutex);s->lifetimes<<std::format("destroy\t{}\n",handle);s->names.erase(handle);}catch(...){s->failed=true;}}
int __cdecl DShader(ID3D12Device* d,void* h){auto r=s->destroy(d,h);if(r==0)Forget(h);return r;}
int __cdecl DFunction(ID3D12Device* d,void* h){auto r=s->destroyFunction(d,h);if(r==0)Forget(h);return r;}
unsigned Start(ID3D12GraphicsCommandList* list) noexcept {
    if(!s->active)return 0;
    if(list!=s->list || s->thread!=GetCurrentThreadId() || s->next+2>(s->generated-1)*8194+8192){s->failed=true;return 0;}
    unsigned first=s->next;s->next+=2;
    if(s->profile)list->EndQuery(s->queries,D3D12_QUERY_TYPE_TIMESTAMP,first);
    return first;
}
void Save(void* function,Dim grid,const void* params,unsigned size,unsigned query,unsigned index,Dim block={},unsigned shared=0) noexcept {
    if(!s->active)return;
    try {
        std::lock_guard lock(s->mutex);
        auto it=s->names.find(function);
        if(it==s->names.end() || size>16384 || (size && !params)){s->failed=true;return;}
        Record r{s->group,s->generated,s->call,index,it->second,grid,{},query,query+1};
        if(block.x) {r.kernel.block=block;r.kernel.shared=shared;}
        auto* bytes=static_cast<const unsigned char*>(params);if(size)r.params.assign(bytes,bytes+size);
        s->records.push_back(std::move(r));
    }catch(...){s->failed=true;}
}
void Stop(ID3D12GraphicsCommandList* list,unsigned first) noexcept {if(s->active){if(s->profile && !s->failed)list->EndQuery(s->queries,D3D12_QUERY_TYPE_TIMESTAMP,first+1);++s->call;}}
int __cdecl RunShader(ID3D12GraphicsCommandList* l,void* h,unsigned x,unsigned y,unsigned z,const void* p,unsigned n){
    auto q=Start(l);Save(h,{x,y,z},p,n,q,0);if(s->failed)return -1;
    auto r=s->run(l,h,x,y,z,p,n);Stop(l,q);return r;
}
int __cdecl Chain(ID3D12GraphicsCommandList* l,const Launch* p,unsigned n){
    auto q=Start(l);if(s->active){if(!p || n>1024){s->failed=true;return -1;}
        for(unsigned i=0;i<n;++i)Save(p[i].function,p[i].grid,p[i].params,p[i].size,q,i,p[i].block,p[i].shared);}
    if(s->failed)return -1;auto r=s->chain(l,p,n);Stop(l,q);return r;
}
int __cdecl ChainEx(ID3D12GraphicsCommandList* l,const LaunchEx* p,unsigned n){
    auto q=Start(l);if(s->active){if(!p || n>1024){s->failed=true;return -1;}
        for(unsigned i=0;i<n;++i){if(p[i].kernelParams){s->failed=true;return -1;}Save(p[i].function,p[i].grid,p[i].params,p[i].size,q,i,p[i].block,p[i].shared);}}
    if(s->failed)return -1;auto r=s->chainEx(l,p,n);Stop(l,q);return r;
}
void* __cdecl Q(unsigned id){
    void* p=s->query(id);
    try{std::lock_guard lock(s->mutex);s->interfaces<<std::format("0x{:08X}\t{}\n",id,p);}catch(...){s->failed=true;}
    if(!p)return p;
#define WRAP(ID,MEMBER,TYPE,FUNC) case ID: if(s->MEMBER && s->MEMBER!=reinterpret_cast<TYPE>(p)){s->failed=true;return p;}s->MEMBER=reinterpret_cast<TYPE>(p);return reinterpret_cast<void*>(&FUNC)
    switch(id){
    WRAP(0x3151211b,create,Create,CEx);
    WRAP(0x1dc7261f,createName,CreateName,CName);
    WRAP(0x2a2c79e8,createBasic,CreateBasic,CBasic);
    WRAP(0x299f5fdc,createV2,CreateV2,CV2);
    WRAP(0xe2436e22,createFunction,CreateFunction,CFunction);
    WRAP(0x5c52bb86,run,Run,RunShader);
    WRAP(0x24973538,chain,RunChain,Chain);
    WRAP(0x846a9bf0,chainEx,RunChainEx,ChainEx);
    WRAP(0x7fb785ba,destroy,Destroy,DShader);
    WRAP(0xdf295ea6,destroyFunction,Destroy,DFunction);
    default:return p;}
#undef WRAP
}
FARPROC WINAPI Resolve(HMODULE m,LPCSTR name){
    auto p=s->resolve(m,name);
    if(m==s->nvapi && reinterpret_cast<uintptr_t>(name)>65535 && !std::strcmp(name,"nvapi_QueryInterface")){
        if(p!=reinterpret_cast<FARPROC>(s->query)){s->failed=true;return p;}return reinterpret_cast<FARPROC>(&Q);
    }return p;
}
}
namespace FGObserver {
void Install(HMODULE provider,const std::filesystem::path& output,bool profile){
    if(s)throw std::runtime_error("Observer already installed");
    s=new State;s->profile=profile;
    s->trace.open(output/"launches.tsv");s->interfaces.open(output/"interfaces.tsv");s->lifetimes.open(output/"lifetimes.tsv");
    if(!s->trace || !s->interfaces || !s->lifetimes)throw std::runtime_error("Cannot open observer outputs");
    s->trace<<"group\tgenerated\tcall\tindex\tgeneration\tname\tgrid\tblock\tshared\tcall_gpu_ms\tparams_hex\n";
    s->nvapi=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!s->nvapi)throw std::runtime_error("NVAPI missing");
    s->query=reinterpret_cast<Query>(GetProcAddress(s->nvapi,"nvapi_QueryInterface"));
    if(!s->query)throw std::runtime_error("NVAPI query missing");
    void** slot{};auto* base=reinterpret_cast<std::byte*>(provider);
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for(auto* entry=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);entry->Name;++entry){
        if(!entry->OriginalFirstThunk)continue;
        auto* names=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+entry->OriginalFirstThunk);
        auto* slots=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+entry->FirstThunk);
        for(;names->u1.AddressOfData;++names,++slots){
            if(IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))continue;
            auto* name=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData)->Name;
            if(!std::strcmp(name,"GetProcAddress")){if(slot)throw std::runtime_error("Ambiguous resolver import");slot=reinterpret_cast<void**>(&slots->u1.Function);}
        }
    }
    if(!slot || *slot!=reinterpret_cast<void*>(&GetProcAddress))throw std::runtime_error("Unexpected provider resolver");
    s->resolve=reinterpret_cast<Resolver>(*slot);DWORD old{},ignored{};
    if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old))throw std::runtime_error("Cannot protect resolver");
    InterlockedExchangePointer(slot,reinterpret_cast<void*>(&Resolve));
    if(!VirtualProtect(slot,sizeof(void*),old,&ignored))throw std::runtime_error("Cannot restore resolver protection");
}
void Begin(ID3D12GraphicsCommandList* list,ID3D12QueryHeap* queries,unsigned group,unsigned generated){
    if(!s || s->active || s->failed)throw std::runtime_error("Invalid observer begin");
    s->list=list;s->queries=queries;s->group=group;s->generated=generated;s->call=0;s->next=(generated-1)*8194;
    s->thread=GetCurrentThreadId();if(generated==1)s->records.clear();s->active=true;
}
unsigned End(){s->active=false;if(s->failed)throw std::runtime_error("Observer contract failed; command list must not execute");return s->profile?s->next-(s->generated-1)*8194:0;}
void Flush(const uint64_t* times,uint64_t frequency){
    for(auto& r:s->records){
        std::string bytes;bytes.reserve(r.params.size()*2);for(auto v:r.params)bytes+=std::format("{:02X}",v);
        auto b=r.kernel.block,g=r.grid;
        s->trace<<std::format("{}\t{}\t{}\t{}\t{}\t{}\t{},{},{}\t{},{},{}\t{}\t{}\t{}\n",r.group,r.generated,r.call,r.index,r.kernel.generation,r.kernel.name,g.x,g.y,g.z,b.x,b.y,b.z,r.kernel.shared,
            s->profile?std::to_string(double(times[r.end]-times[r.start])*1000.0/double(frequency)):"",bytes);
    }
    s->trace.flush();s->interfaces.flush();s->lifetimes.flush();
}
void Finish(){if(s){std::lock_guard lock(s->mutex);s->trace.flush();s->interfaces.flush();s->lifetimes.flush();if(s->failed)throw std::runtime_error("Observer failed during shutdown");}}
}
