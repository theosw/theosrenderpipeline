// Exercise the production wrappers with vendor doubles; no GPU dispatch.
#include "../../extern/MFGAmpere/runtime.cpp"
#include "../../src/FrameGen/SourceDLSSGMFG.h"
#include <thread>
#include <iostream>
#include <map>
using namespace trp::ampere;
unsigned checks{};
void Require(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
struct Page {
    std::uint8_t* p=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    Page() { Require(p!=nullptr,"allocate fixture page"); }
    ~Page() { VirtualFree(p,0,MEM_RELEASE); }
    void Protect(DWORD value) { DWORD old{}; Require(VirtualProtect(p,4096,value,&old)!=FALSE,"protect page"); }
    DWORD Protection() { MEMORY_BASIC_INFORMATION m{}; Require(VirtualQuery(p,&m,sizeof(m))==sizeof(m),"query page");return m.Protect; }
};
void Transactions() {
    const std::array<std::uint8_t,3> before{1,2,3},after{4,5,6};
    Page page; std::memcpy(page.p,before.data(),3); page.Protect(PAGE_READONLY);
    memory::Transaction t; t.Add(page.p,before,after);
    Require(t.Commit(),"commit read-only data"); Require(memory::Equal(page.p,after),"write retained");
    Require(page.Protection()==PAGE_READONLY,"restore protection"); Require(!t.Commit(),"no repeated commit");
    Require(t.Rollback() && !t.Unsafe(),"rollback succeeds");Require(memory::Equal(page.p,before),"rollback original bytes");
    Require(t.Rollback(),"rollback idempotent");
    memory::Transaction mismatch; mismatch.Add(page.p,after,before);
    Require(!mismatch.Commit() && !mismatch.Unsafe(),"mismatch refuses before mutation");Require(memory::Equal(page.p,before),"mismatch leaves original");
    Page code; std::memcpy(code.p,before.data(),3);code.Protect(PAGE_EXECUTE_READ);
    memory::Transaction c;c.Add(code.p,before,after);Require(c.Commit(),"code publication");Require(code.Protection()==PAGE_EXECUTE_READ,"code protection");Require(c.Rollback(),"code rollback");
    Page pointers;std::uintptr_t p=0x12345678,q=0x87654321;std::memcpy(pointers.p,&p,8);pointers.Protect(PAGE_READONLY);
    memory::Transaction ptr;ptr.Add(pointers.p,{reinterpret_cast<std::uint8_t*>(&p),8},{reinterpret_cast<std::uint8_t*>(&q),8});
    Require(ptr.Commit(),"aligned pointer publication");std::uintptr_t observed{};memory::Read(pointers.p,observed);Require(observed==q,"published pointer");
    Require(ptr.Rollback(),"pointer rollback");memory::Read(pointers.p,observed);Require(observed==p,"original pointer");
    memory::Transaction ownership;ownership.Add(page.p,before,after);Require(ownership.Commit(),"ownership setup");
    page.Protect(PAGE_READWRITE);page.p[0]=9;page.Protect(PAGE_READONLY);
    Require(!ownership.Rollback() && ownership.Unsafe() && ownership.Count()==1,"foreign replacement retains rollback record");Require(page.p[0]==9,"foreign replacement not overwritten");
}
struct Params final : NVSDK_NGX_Parameter {
    std::map<std::string,int> values;
    std::string rejectWrite;
    void Set(const char*,unsigned long long) override {} void Set(const char*,float) override {} void Set(const char*,double) override {}
    void Set(const char* key,unsigned int v) override { values[key]=static_cast<int>(v); }
    void Set(const char* key,int v) override {if(rejectWrite != key) values[key]=v;}
    void Set(const char*,ID3D11Resource*) override {} void Set(const char*,ID3D12Resource*) override {} void Set(const char*,void*) override {}
    NVSDK_NGX_Result Get(const char*,unsigned long long*) const override {return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,float*) const override {return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,double*) const override {return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char* key,unsigned int* out) const override {int v{};auto r=Get(key,&v);*out=static_cast<unsigned int>(v);return r;}
    NVSDK_NGX_Result Get(const char* key,int* out) const override {auto i=values.find(key);if(i==values.end())return NVSDK_NGX_Result_FAIL_InvalidParameter;*out=i->second;return NVSDK_NGX_Result_Success;}
    NVSDK_NGX_Result Get(const char*,ID3D11Resource**) const override {return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,ID3D12Resource**) const override {return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,void**) const override {return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    void Reset() override {values.clear();}
};
int __cdecl RealArch(void*,ArchInfo* info) {if(info)info->architecture=kAmpere;return 0;}
void Policies() {
    using namespace TheosRenderPipeline::SourceDLSSG;
    for(auto adapter:{midpoint_fix::AdapterKind::Ada,midpoint_fix::AdapterKind::Ampere,midpoint_fix::AdapterKind::Other,midpoint_fix::AdapterKind::Unavailable}) {
        MFGSnapshot state;Require(state.SelectRoute(adapter) && state.route==MFGRoute::Native,"disabled compatibility stays native");
        state.requested=true;const bool ok=state.SelectRoute(adapter);
        Require(ok==(adapter!=midpoint_fix::AdapterKind::Unavailable),"unavailable physical adapter fails");
        if(ok) Require(state.route==(adapter==midpoint_fix::AdapterKind::Ada ? MFGRoute::AdaUnlock : adapter==midpoint_fix::AdapterKind::Ampere ? MFGRoute::AmpereUnlock : MFGRoute::Native),"physical architecture selects route");
        Require(!state.Ready(),"routing alone cannot establish readiness");
    }
    auto& s=State();s.arch=RealArch;s.gpu=reinterpret_cast<void*>(1);
    ArchInfo a{0x20010,0,0,0};s.prepared=true;s.installed=true;
    Architecture(s.gpu,&a);Require(a.architecture==kAmpere,"outside scope remains physical");
    {ExposureScope scope(true);Architecture(s.gpu,&a);Require(a.architecture==kAda,"prepared matching GPU exposed in scope");
      Architecture(reinterpret_cast<void*>(2),&a);Require(a.architecture==kAmpere,"other GPU remains physical");
      {ExposureScope suppress(false);Architecture(s.gpu,&a);Require(a.architecture==kAmpere,"non-FG nested scope suppressed");}
      Architecture(s.gpu,&a);Require(a.architecture==kAda,"nested scope restored");
      a.version=0x30010;Architecture(s.gpu,&a);Require(a.architecture==kAmpere,"unknown ABI remains unchanged");a.version=0x20010;
      s.prepared=false;Architecture(s.gpu,&a);Require(a.architecture==kAmpere,"unprepared provider remains physical");s.prepared=true;}
    for(int available:{0,1})for(int driver:{0,1})for(int maximum:{0,1,5,9}) {
      Params p;p.values={{"FrameGeneration.Available",available},{"FrameGeneration.NeedsUpdatedDriver",driver},{"DLSSG.MultiFrameCountMax",maximum}};
      UpdateCapabilities(&p);const bool enabled=available || !driver;
      Require(p.values["FrameGeneration.Available"]==int(enabled),"availability respects driver failure");
      Require(p.values["DLSSG.MultiFrameCountMax"]==(enabled && maximum<5 ? 5 : maximum),"capacity raises only available provider and preserves higher limits");
    }
    Params failed;failed.values={{"FrameGeneration.Available",0},{"FrameGeneration.NeedsUpdatedDriver",0},{"FrameGeneration.FeatureInitResult",int(NVSDK_NGX_Result_FAIL_InvalidParameter)},{"DLSSG.MultiFrameCountMax",1}};
    UpdateCapabilities(&failed);Require(failed.values["FrameGeneration.Available"]==0 && failed.values["DLSSG.MultiFrameCountMax"]==1,"unrelated init failure preserved");
    s.prepared=false;
    Params noReady;noReady.values={{"FrameGeneration.Available",0},{"FrameGeneration.NeedsUpdatedDriver",0},{"DLSSG.MultiFrameCountMax",1}};UpdateCapabilities(&noReady);Require(noReady.values["FrameGeneration.Available"]==0,"no exposure before preparation");
}
// Each startup case runs in a separate process. State injected below belongs
// only to vendor doubles; these checks never impersonate a physical adapter.
std::vector<std::string> messages;
void CaptureLog(const char* text) { messages.emplace_back(text); }
bool Saw(const char* text) { return std::any_of(messages.begin(),messages.end(),[&](const auto& s){return s.find(text)!=std::string::npos;}); }
struct Adapter final : IDXGIAdapter {
    LUID luid{17,3}; UINT vendor=0x10de; HRESULT status=S_OK;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override {return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetParent(REFIID,void**) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT,IDXGIOutput**) override {return DXGI_ERROR_NOT_FOUND;}
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* out) override {if(FAILED(status))return status;*out={};out->AdapterLuid=luid;out->VendorId=vendor;return S_OK;}
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID,LARGE_INTEGER*) override {return E_NOTIMPL;}
};
struct Published {
    std::uintptr_t descriptor=0x12345678;
    std::array<std::uint8_t,6> arch{0xb8,0x70,1,0,0,0xc3};
    std::array<Resolver,2> imports{Resolve<0>,Resolve<1>};
    Published() {
        auto& s=State();s.log=CaptureLog;s.prepared=true;s.installed=true;s.arch=RealArch;s.gpu=reinterpret_cast<void*>(1);s.luid={17,3};
        s.temporal.slot=reinterpret_cast<std::uintptr_t>(&descriptor);s.temporal.replacementDescriptor=descriptor;
        s.minimumArch=arch.data();for(unsigned i=0;i<2;++i)s.importSlots[i]=reinterpret_cast<void**>(&imports[i]);
    }
};
NVSDK_NGX_Result vendorResult=NVSDK_NGX_Result_Success;
NVSDK_NGX_FeatureRequirement vendorRequirement{};
Params vendorParams;
unsigned vendorCalls{}, observedArchitecture{};
IDXGIAdapter* observedAdapter{};const NVSDK_NGX_FeatureDiscoveryInfo* observedDiscovery{};
ID3D12GraphicsCommandList* observedCommands{}; NVSDK_NGX_Feature observedFeature{};
NVSDK_NGX_Parameter* observedParams{}; NVSDK_NGX_Handle** observedHandle{};
NVSDK_NGX_Handle vendorHandle{};
void ObserveArchitecture() {ArchInfo a{0x20010,0,0,0};Architecture(State().gpu,&a);observedArchitecture=a.architecture;}
NVSDK_NGX_Result NVSDK_CONV MockRequirements(IDXGIAdapter* adapter,const NVSDK_NGX_FeatureDiscoveryInfo* discovery,NVSDK_NGX_FeatureRequirement* out) {
    ++vendorCalls;observedAdapter=adapter;observedDiscovery=discovery;ObserveArchitecture();if(out)*out=vendorRequirement;return vendorResult;
}
NVSDK_NGX_Result NVSDK_CONV MockParameters(NVSDK_NGX_Parameter** out) {++vendorCalls;ObserveArchitecture();if(out)*out=&vendorParams;return vendorResult;}
NVSDK_NGX_Result NVSDK_CONV MockCreate(ID3D12GraphicsCommandList* commands,NVSDK_NGX_Feature feature,NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** out) {
    ++vendorCalls;observedCommands=commands;observedFeature=feature;observedParams=params;observedHandle=out;ObserveArchitecture();
    if(out)*out=vendorResult==NVSDK_NGX_Result_Success ? &vendorHandle : nullptr;
    return vendorResult;
}
void RequirementCalls() {
    Published published;Adapter adapter;auto& s=State();
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};discovery.FeatureID=NVSDK_NGX_Feature_FrameGeneration;
    NVSDK_NGX_FeatureRequirement out{};
    Require(GetRequirements(&adapter,&discovery,&out)==NVSDK_NGX_Result_FAIL_InvalidParameter && vendorCalls==0,"missing requirements function cannot succeed");
    s.requirements=MockRequirements;
    for(bool ready:{false,true})for(bool fg:{false,true})for(int identity:{0,1,2,3})for(unsigned flags:{0u,1u,2u,4u,5u,6u}) {
        s.prepared=ready;adapter.luid={identity==1 ? 18u:17u,3};adapter.vendor=identity==2 ? 0x1002:0x10de;adapter.status=identity==3 ? E_FAIL:S_OK;
        discovery.FeatureID=fg ? NVSDK_NGX_Feature_FrameGeneration:NVSDK_NGX_Feature_SuperSampling;
        vendorRequirement={};vendorRequirement.FeatureSupported=static_cast<NVSDK_NGX_Feature_Support_Result>(flags);vendorRequirement.MinHWArchitecture=kAda;
        const auto previous=vendorCalls;
        Require(GetRequirements(&adapter,&discovery,&out)==NVSDK_NGX_Result_Success && vendorCalls==previous+1,"requirements forwarded exactly once");
        Require(observedAdapter==&adapter && observedDiscovery==&discovery,"requirements argument identity retained");
        const bool eligible=ready && fg && identity==0;
        Require(observedArchitecture==(eligible ? kAda:kAmpere),"architecture exposure limited to prepared matching FG call");
        const bool amend=eligible && (flags==0 || flags==4);
        Require(static_cast<unsigned>(out.FeatureSupported)==(amend ? 0:flags) && out.MinHWArchitecture==(amend ? kAmpere:kAda),"OS/driver/identity failures retained");
    }
    s.prepared=true;adapter.luid=s.luid;adapter.vendor=0x10de;adapter.status=S_OK;discovery.FeatureID=NVSDK_NGX_Feature_FrameGeneration;
    vendorRequirement.FeatureSupported=static_cast<NVSDK_NGX_Feature_Support_Result>(4);vendorRequirement.MinHWArchitecture=kAda;
    vendorResult=NVSDK_NGX_Result_FAIL_InvalidParameter;
    Require(GetRequirements(&adapter,&discovery,&out)==vendorResult && out.MinHWArchitecture==kAda && static_cast<unsigned>(out.FeatureSupported)==4,"failed requirements result and payload preserved");
    vendorResult=NVSDK_NGX_Result_Success;vendorRequirement.MinHWArchitecture=0x1b0;
    GetRequirements(&adapter,&discovery,&out);Require(out.MinHWArchitecture==0x1b0 && static_cast<unsigned>(out.FeatureSupported)==4,"unrecognized minimum architecture preserved");
    GetRequirements(nullptr,&discovery,&out);Require(observedArchitecture==kAmpere,"null adapter cannot receive exposure");
    GetRequirements(&adapter,nullptr,&out);Require(observedArchitecture==kAmpere,"null discovery cannot receive exposure");
    Require(GetRequirements(&adapter,&discovery,nullptr)==vendorResult,"null requirements output forwarded without dereference");
    Require(startupScope==0 && !suppressExposure,"requirements scope restored");
}
void ParameterCalls(const std::string& mode) {
    Published published;auto& s=State();NVSDK_NGX_Parameter* output=nullptr;
    Require(GetParameters<0>(&output)==NVSDK_NGX_Result_FAIL_InvalidParameter && vendorCalls==0,"missing capability export cannot succeed");
    Require(GetParameters<1>(&output)==NVSDK_NGX_Result_FAIL_InvalidParameter && vendorCalls==0,"missing parameter export cannot succeed");
    s.parameters[0]=MockParameters;s.parameters[1]=MockParameters;
    vendorParams.values={{"FrameGeneration.Available",0},{"FrameGeneration.NeedsUpdatedDriver",0},{"DLSSG.MultiFrameCountMax",1}};
    if(mode=="capabilities") {
        for(unsigned variant:{0u,1u}) {
            vendorResult=NVSDK_NGX_Result_FAIL_InvalidParameter;const auto before=vendorParams.values;
            const auto result=variant ? GetParameters<1>(&output):GetParameters<0>(&output);
            Require(result==vendorResult && output==&vendorParams && vendorParams.values==before,"failed parameter call is not rewritten");
        }
        vendorResult=NVSDK_NGX_Result_Success;s.installed=false;
        GetParameters<0>(&output);Require(vendorParams.values["FrameGeneration.Available"]==0 && observedArchitecture==kAmpere,"incomplete bridge cannot advertise support");
        s.installed=true;GetParameters<1>(&output);
        Require(output==&vendorParams && vendorParams.values["FrameGeneration.Available"]==1 && vendorParams.values["DLSSG.MultiFrameCountMax"]==5,"prepared capability result and object retained");
        Require(GetParameters<0>(nullptr)==NVSDK_NGX_Result_Success,"null parameter output forwarded");
        Require(!Snapshot().failed,"normal forwarding does not latch failure");
    } else {
        vendorParams.rejectWrite=mode=="capabilities-reject-availability" ? "FrameGeneration.Available":"DLSSG.MultiFrameCountMax";
        GetParameters<0>(&output);
        Require(Snapshot().failed && Snapshot().error && Saw("did not retain"),"vendor refusing a required value latches an explicit failure");
        Require(!Prepared() && !Verify(),"rejected preparation cannot remain ready");
        ArchInfo a{0x20010,0,0,0};{ExposureScope scope(true);Architecture(s.gpu,&a);}Require(a.architecture==kAmpere,"failed owner stops architecture exposure");
    }
    Require(startupScope==0 && !suppressExposure,"parameters scope restored");
}
void CreationCalls(const std::string& mode) {
    Published published;auto& s=State();NVSDK_NGX_Handle* output=&vendorHandle;
    auto* commands=reinterpret_cast<ID3D12GraphicsCommandList*>(0x1000);
    const auto fg=NVSDK_NGX_Feature_FrameGeneration;
    Require(CreateFeature(commands,fg,&vendorParams,&output)==NVSDK_NGX_Result_FAIL_InvalidParameter && vendorCalls==0,"missing create export cannot succeed or dispatch");
    s.create=MockCreate;
    if(mode=="create-ready") {
        Require(Verify(),"fixture publication complete");
        vendorResult=NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
        Require(CreateFeature(commands,fg,&vendorParams,&output)==vendorResult && output==nullptr && vendorCalls==1,"vendor create error preserved without fabricated handle");
        Require(Saw("creation failed") && Snapshot().createSeen && Snapshot().createCalls==1,"failed vendor create is visible and records consumer boundary");
        Require(observedArchitecture==kAda && observedCommands==commands && observedFeature==fg && observedParams==&vendorParams && observedHandle==&output,"FG create ABI and scoped exposure preserved");
        vendorResult=NVSDK_NGX_Result_Success;
        Require(CreateFeature(commands,fg,&vendorParams,&output)==vendorResult && output==&vendorHandle && vendorCalls==2,"successful vendor create forwards exact handle");
        Require(Saw("feature created") && !Snapshot().failed,"successful forwarding recorded");
        {ExposureScope startup(true);Require(CreateFeature(commands,NVSDK_NGX_Feature_SuperSampling,&vendorParams,&output)==vendorResult,"non-FG create forwarded inside startup scope");Require(observedArchitecture==kAmpere,"non-FG create suppresses nested exposure");}
        Require(Snapshot().createCalls==2 && vendorCalls==3,"non-FG call excluded from FG counters");
    } else {
        if(mode=="create-unprepared")s.prepared=false;
        else if(mode=="create-bridge-missing")s.installed=false;
        else if(mode=="create-failed-owner")Fail("previous startup failure");
        else if(mode=="create-descriptor-changed")published.descriptor++;
        else if(mode=="create-arch-changed")published.arch[1]=0x90;
        else if(mode=="create-import-changed")published.imports[1]=nullptr;
        else throw std::runtime_error("unknown creation fixture");
        Require(CreateFeature(commands,fg,&vendorParams,&output)==NVSDK_NGX_Result_FAIL_FeatureNotSupported && output==nullptr && vendorCalls==0,"incomplete or damaged preparation blocks vendor create and clears handle");
        Require(Snapshot().createSeen && Snapshot().createCalls==1 && !Verify(),"failed create boundary cannot report readiness");
        if(mode.find("changed")!=std::string::npos)Require(Snapshot().failed && Snapshot().error && Saw("publication changed"),"publication failure recorded");
        if(mode=="create-failed-owner") {Fail("secondary failure");Require(std::string(Snapshot().error)=="previous startup failure","first causal failure retained");}
    }
    Require(startupScope==0 && !suppressExposure,"create scope restored on all exits");
}
FARPROC resolverOutput{};
FARPROC WINAPI MockResolver(HMODULE,LPCSTR) {++vendorCalls;return resolverOutput;}
void ResolverCalls() {
    auto& s=State();s.resolvers={MockResolver,MockResolver};s.log=CaptureLog;
    const char* name="NVSDK_NGX_D3D12_CreateFeature";
    Require(Resolve<0>(nullptr,name)==nullptr && !s.create.load() && s.mask.load()==0,"missing vendor export remains missing");
    resolverOutput=reinterpret_cast<FARPROC>(MockCreate);
    Require(Resolve<0>(nullptr,"NVSDK_NGX_D3D11_CreateFeature")==resolverOutput && !s.create.load(),"D3D11 DLSS SR resolver remains untouched");
    Require(Resolve<0>(nullptr,"unrelated_export")==resolverOutput && !s.create.load(),"unrelated export forwarded unchanged");
    Require(Resolve<0>(nullptr,reinterpret_cast<LPCSTR>(17))==resolverOutput,"ordinal export forwarded without string access");
    Require(Resolve<1>(nullptr,name)==resolverOutput && !s.create.load(),"wrapper resolver does not intercept NGX core calls");
    Require(Resolve<0>(nullptr,name)==resolverOutput && Snapshot().failed && !s.create.load() && Saw("module identity"),"wrong core identity cannot bind bridge function");
    std::atomic<Create> binding{};Require(Bind(binding,resolverOutput) && Bind(binding,resolverOutput),"same export binding is idempotent");
    const auto different=reinterpret_cast<FARPROC>(CreateFeature);
    Require(!Bind(binding,different) && binding.load()==MockCreate,"changed export cannot replace retained original");
}
void StartupReentry() {
    Require(!Start(nullptr,std::filesystem::path("relative"),CaptureLog),"invalid startup device/path refused");
    Require(Snapshot().failed && Snapshot().error && !Snapshot().prepared && !Snapshot().bridgeInstalled && !Snapshot().createSeen,"invalid startup leaves provider unpublished");
    Require(!Start(nullptr,std::filesystem::absolute("."),CaptureLog) && Saw("cannot be repeated"),"startup cannot be repeated after failure");
    Require(std::string(Snapshot().error)=="Ampere preparation requires the actual SM86 rendering adapter","startup preserves first failure");
}

void RepeatedCreation() {
    Published published;
    auto& s = State();
    s.create = MockCreate;
    const auto descriptor = published.descriptor;
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        NVSDK_NGX_Handle* output = nullptr;
        Require(CreateFeature(nullptr, NVSDK_NGX_Feature_FrameGeneration, &vendorParams, &output) == NVSDK_NGX_Result_Success,
            "repeated FG create must forward to vendor");
        Require(output == &vendorHandle && observedArchitecture == kAda && s.createCalls == cycle + 1,
            "each FG create retains the scoped exposure and exact vendor result");
        Require(Verify() && published.descriptor == descriptor,
            "feature recreation must retain the same immutable provider publication");
        {
            ExposureScope outer(true);
            Require(CreateFeature(nullptr, NVSDK_NGX_Feature_SuperSampling, &vendorParams, &output) == NVSDK_NGX_Result_Success,
                "SR create during host startup must still forward");
            Require(observedArchitecture == kAmpere, "SR must see the physical architecture even inside startup");
            unsigned peerArchitecture{};
            std::thread peer([&] {
                ArchInfo info{0x20010, 0, 0, 0};
                Architecture(s.gpu, &info);
                peerArchitecture = info.architecture;
            });
            peer.join();
            Require(peerArchitecture == kAmpere, "FG exposure must not cross threads");
            ObserveArchitecture();
            Require(observedArchitecture == kAda, "nested SR call must restore the caller scope");
        }
        ObserveArchitecture();
        Require(observedArchitecture == kAmpere && startupScope == 0 && !suppressExposure,
            "each recreation must leave physical architecture outside FG scope");
    }
    published.descriptor++;
    NVSDK_NGX_Handle* output = &vendorHandle;
    const auto calls = vendorCalls;
    Require(CreateFeature(nullptr, NVSDK_NGX_Feature_FrameGeneration, &vendorParams, &output) == NVSDK_NGX_Result_FAIL_FeatureNotSupported
        && !output && vendorCalls == calls, "changed publication must stop the next create before vendor dispatch");
}

int main(int argc,char** argv) {try {
    Require(argc==2,"runtime_tests <case>");const std::string mode=argv[1];
    if(mode=="policies"){Transactions();Policies();}
    else if(mode=="requirements")RequirementCalls();
    else if(mode.starts_with("capabilities"))ParameterCalls(mode);
    else if(mode.starts_with("create-"))CreationCalls(mode);
    else if(mode=="resolvers")ResolverCalls();
    else if(mode=="repeated-create")RepeatedCreation();
    else if(mode=="startup-reentry")StartupReentry();
    else throw std::runtime_error("unknown fixture case");
    std::cout<<"PASS "<<checks<<" checks; no GPU dispatch or Ampere hardware claim"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks"<<std::endl;return 1;}}
