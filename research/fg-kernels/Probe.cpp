// Standalone, native-adapter FG oracle. No Skyrim, Streamline host, or injection.
#include "GpuContext.h"
#include "Observer.h"
#include "FrameGen/NeuralRenderingRuntimeIdentity.h"
#include "FrameGen/SourceDLSSGMFGPatch.h"
#include "midpoint_fix.h"
#include <nvsdk_ngx.h>
#include <DirectXPackedVector.h>
#include <set>

struct Parameters final : NVSDK_NGX_Parameter {
    NVSDK_NGX_Parameter* real{};
    mutable std::ofstream trace;
    mutable std::set<std::string> seen;
    std::string stage = "populate";
    Parameters(NVSDK_NGX_Parameter* p, const fs::path& path) : real(p), trace(path) {
        trace << "stage\toperation\tname\ttype\tstatus\n";
    }
    void record(const char* op, const char* name, const char* type, NVSDK_NGX_Result r) const {
        auto key = std::format("{}\t{}\t{}\t{}\t0x{:08X}",stage,op,name,type,unsigned(r));
        if (seen.insert(key).second) { trace << key << std::endl; }
    }
#define PARAM(T) \
    void Set(const char* n,T v) override {real->Set(n,v);record("Set",n,#T,NVSDK_NGX_Result_Success);} \
    NVSDK_NGX_Result Get(const char* n,T* v) const override {auto r=real->Get(n,v);record("Get",n,#T,r);return r;}
    PARAM(unsigned long long)
    PARAM(float)
    PARAM(double)
    PARAM(unsigned int)
    PARAM(int)
    PARAM(ID3D11Resource*)
    PARAM(ID3D12Resource*)
    PARAM(void*)
#undef PARAM
    void Reset() override {real->Reset();}
};
using Init = NVSDK_NGX_Result(__cdecl*)(unsigned long long,const wchar_t*,ID3D12Device*,NVSDK_NGX_Version,const void*);
using Populate = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter*);
using Create = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using Evaluate = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback_C);
using Release = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Handle*);
using Shutdown = NVSDK_NGX_Result(__cdecl*)();
template<class T>T symbol(HMODULE h,const char* name) {auto p=GetProcAddress(h,name);require(p!=nullptr,name);return reinterpret_cast<T>(p);}
void ngx(NVSDK_NGX_Result r,const char* stage) {
    std::cerr<<std::format("{}=0x{:08X}\n",stage,unsigned(r));
    require(NVSDK_NGX_SUCCEED(r),stage);
}

int wmain(int argc,wchar_t** argv) try {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if(argc!=9) {std::cerr<<"FGProbe <provider> <new-output-dir> <width> <height> <frames> <baseline|observe|profile> <2|4> <static|move|recreate>\n";return 64;}
    auto dll=fs::absolute(argv[1]),out=fs::absolute(argv[2]);
    const unsigned w=std::stoul(argv[3]),h=std::stoul(argv[4]),frames=std::stoul(argv[5]);
    const std::wstring mode=argv[6];require(mode==L"baseline"||mode==L"observe"||mode==L"profile","Invalid observation mode");
    const bool observe=mode!=L"baseline";
    const unsigned multiplier=std::stoul(argv[7]);require(multiplier==2||multiplier==4,"Only x2/x4 are measured");
    const std::wstring fixture=argv[8];require(fixture==L"static"||fixture==L"move"||fixture==L"recreate","Invalid fixture");
    require(w>=128 && h>=128 && w<=8192 && h<=4096 && frames>0 && frames<=256,"Invalid extent/frame count");
    require(!fs::exists(out),"Output already exists");fs::create_directories(out);
    namespace Identity=TheosRenderPipeline::NeuralRenderingRuntimeIdentity;
    const auto id=Identity::VerifyExpected(dll,7460976,"FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82");
    require(id.matched,"Unrecognized provider");
    std::ofstream manifest(out/"identity.txt");manifest<<id.path.string()<<'\n'<<id.version<<'\n'<<id.sha256<<'\n';
    const auto executable=Identity::VerifyExpected(Identity::ModulePath(nullptr),0,"");
    require(!executable.sha256.empty(),"Cannot identify probe executable");
    manifest<<"probe_sha256="<<executable.sha256<<'\n';
    Context c;
    manifest<<c.adapterIdentity<<'\n';
    manifest<<std::format("dimensions={}x{} frames={} multiplier={}\n",w,h,frames,multiplier);
    manifest<<"fixture="<<fs::path(fixture).string()<<" mode="<<fs::path(mode).string()<<'\n';
    manifest<<"native adapter; no Ampere spoof; Ada provider patches only for x4\n";manifest.flush();
    auto cache=out/"ngx-cache";fs::create_directories(cache);
    ngx(NVSDK_NGX_D3D12_Init_with_ProjectID("f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "fg-oracle",cache.c_str(),c.device.Get(),nullptr,NVSDK_NGX_Version_API),"NGX bootstrap");
    NVSDK_NGX_Parameter* raw{};ngx(NVSDK_NGX_D3D12_AllocateParameters(&raw),"Allocate parameters");
    Parameters p(raw,out/"parameters.tsv");
    auto module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    require(module && Identity::EqualPath(Identity::ModulePath(module),dll),"Provider path mismatch");
    if(observe)FGObserver::Install(module,out,mode==L"profile");
    auto init=symbol<Init>(module,"NVSDK_NGX_D3D12_Init_Ext");
    auto populate=symbol<Populate>(module,"NVSDK_NGX_D3D12_PopulateParameters_Impl");
    auto create=symbol<Create>(module,"NVSDK_NGX_D3D12_CreateFeature");
    auto evaluate=symbol<Evaluate>(module,"NVSDK_NGX_D3D12_EvaluateFeature");
    auto release=symbol<Release>(module,"NVSDK_NGX_D3D12_ReleaseFeature");
    auto shutdown=symbol<Shutdown>(module,"NVSDK_NGX_D3D12_Shutdown");
    ngx(init(0x0876232C,cache.c_str(),c.device.Get(),NVSDK_NGX_Version_API,nullptr),"Provider init");
    if(multiplier>2){
        namespace Contract=TheosRenderPipeline::SourceDLSSG::MFGContract;
        namespace Patch=TheosRenderPipeline::SourceDLSSG::MFGPatch;
        require(midpoint_fix::ObserveD3D12Device(c.device.Get()),"x4 oracle requires a verified Ada adapter");
        midpoint_fix::SetLogCallback([](const wchar_t* m){std::wcerr<<m<<std::endl;});
        auto* target=Patch::FindExecutable(module,Contract::providerPattern.size(),Contract::MatchesProvider);
        require(target && Patch::ProviderBranchMatches(module,target),"Unknown provider capability branch");
        bool unsafe=false;require(Patch::WriteCode(target+2,Contract::providerOriginal,Contract::providerReplacement,unsafe),"Capability patch failed");
        require(midpoint_fix::PatchProvider(module,dll.c_str()),"Temporal program not ready");
    }
    ngx(populate(&p),"Populate");
    p.stage="create";
    p.Set("DLSSG.Width",w);p.Set("DLSSG.Height",h);p.Set("Width",w);p.Set("Height",h);
    p.Set("DLSSG.InternalWidth",w);p.Set("DLSSG.InternalHeight",h);
    p.Set("DLSSG.BackbufferFormat",unsigned(DXGI_FORMAT_R16G16B16A16_FLOAT));
    p.Set("DLSSG.DynamicResolution",0u);p.Set("DLSSG.UseReflexMatrices",0u);
    p.Set("CreationNodeMask",1u);p.Set("VisibilityNodeMask",1u);
    NVSDK_NGX_Handle* feature{};c.begin();
    ngx(create(c.list.Get(),NVSDK_NGX_Feature_FrameGeneration,&p,&feature),"Create FG");c.finish();
    require(feature!=nullptr,"Feature handle missing");
    auto color=c.texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT),output=c.texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto backbuffer=c.texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto real=c.texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto depth=c.texture(w,h,DXGI_FORMAT_R32_FLOAT),motion=c.texture(w,h,DXGI_FORMAT_R16G16_FLOAT);
    color->SetName(L"FG input color");output->SetName(L"FG interpolated output");real->SetName(L"FG real output");
    depth->SetName(L"FG depth");motion->SetName(L"FG motion");
    backbuffer->SetName(L"FG input backbuffer");
    c.begin();c.transition(output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.transition(real.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);c.finish();
    float identity[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    p.stage="evaluate";
    p.Set("DLSSG.Backbuffer",backbuffer.Get());p.Set("DLSSG.HUDLess",color.Get());
    p.Set("DLSSG.Depth",depth.Get());p.Set("DLSSG.MVecs",motion.Get());
    p.Set("DLSSG.OutputInterpolated",output.Get());p.Set("DLSSG.OutputReal",real.Get());
    for(auto name:{"CameraViewToClip","ClipToCameraView","ClipToLensClip","ClipToPrevClip","PrevClipToClip"}) p.Set((std::string("DLSSG.")+name).c_str(),static_cast<void*>(identity));
    for(auto name:{"Backbuffer","HUDLess","Depth","MVecs","OutputInterpolated","OutputReal"}) {
        auto prefix=std::string("DLSSG.")+name+"Subrect";
        p.Set((prefix+"BaseX").c_str(),0u);p.Set((prefix+"BaseY").c_str(),0u);
        p.Set((prefix+"Width").c_str(),w);p.Set((prefix+"Height").c_str(),h);
    }
    for(auto name:{"JitterOffsetX","JitterOffsetY","CameraPosX","CameraPosY","CameraPosZ","CameraFwdX","CameraFwdY","CameraRightY","CameraRightZ","CameraUpX","CameraUpZ","CameraPinholeOffsetX","CameraPinholeOffsetY"})p.Set((std::string("DLSSG.")+name).c_str(),0.f);
    for(auto name:{"CameraRightX","CameraUpY","CameraFwdZ"})p.Set((std::string("DLSSG.")+name).c_str(),1.f);
    p.Set("DLSSG.CameraAspectRatio",float(w)/h);p.Set("DLSSG.CameraFOV",1.04719755f);
    p.Set("DLSSG.CameraNear",0.1f);p.Set("DLSSG.CameraFar",1000.f);
    p.Set("DLSSG.MvecScaleX",1.f);p.Set("DLSSG.MvecScaleY",1.f);p.Set("DLSSG.CameraMotionIncluded",1u);
    p.Set("DLSSG.MultiFrameCount",multiplier-1);
    p.Set("DLSSG.ColorBuffersHDR",0u);p.Set("DLSSG.DepthInverted",0u);p.Set("DLSSG.MvecDilated",0u);
    p.Set("DLSSG.MvecJittered",0u);p.Set("DLSSG.NotRenderingGameFrames",0u);
    D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=8194*(multiplier-1)+2;
    ComPtr<ID3D12QueryHeap> queries;check(c.device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries)));
    auto times=c.buffer(UINT64(q.Count)*8,D3D12_HEAP_TYPE_READBACK);
    UINT64 readSize{};auto layout=c.footprint(output.Get(),readSize);
    std::vector<ComPtr<ID3D12Resource>> readbacks;
    for(unsigned j=1;j<multiplier;++j)readbacks.push_back(c.buffer(readSize,D3D12_HEAP_TYPE_READBACK));
    std::ofstream timing(out/"timings.csv");timing<<"group,generated,evaluate_gpu_ms,launch_queries\n";
    for(unsigned frame=0;frame<frames;++frame) {
        if(fixture==L"recreate" && frame && frame%8==0){
            ngx(release(feature),"Release before recreation");feature=nullptr;
            c.begin();ngx(create(c.list.Get(),NVSDK_NGX_Feature_FrameGeneration,&p,&feature),"Recreate FG");c.finish();
        }
        c.begin();
        std::vector<char> rgba(size_t(w)*h*8),mv(size_t(w)*h*4),db(size_t(w)*h*4);
        for(size_t i=0;i<size_t(w)*h;++i) {
            const unsigned x=unsigned(i%w),y=unsigned(i/w),left=w/4+(fixture==L"static"?0:frame*4);
            const bool bar=x>=left && x<left+w/8 && y>h/6 && y<h*5/6;
            const bool grid=x%31==0 || y%29==0;
            const uint16_t pixel[]{uint16_t(bar?0x3c00:grid?0x3800:0x3000),uint16_t(bar?0x3400:grid?0x3400:0x3800),uint16_t(bar?0x3400:0x3800),0x3c00};
            std::memcpy(rgba.data()+i*8,pixel,8);float z=bar?0.25f:0.75f;std::memcpy(db.data()+i*4,&z,4);
            if(bar && frame && fixture!=L"static"){
                const uint16_t dx=DirectX::PackedVector::XMConvertFloatToHalf(-4.f/float(w));std::memcpy(mv.data()+i*4,&dx,2);
            }
        }
        auto uc=c.upload(color.Get(),rgba,size_t(w)*8),ud=c.upload(depth.Get(),db,size_t(w)*4),um=c.upload(motion.Get(),mv,size_t(w)*4);
        auto ub=c.upload(backbuffer.Get(),rgba,size_t(w)*8);
        c.finish(); // Input transfer stays outside the measured FG evaluation.
        write(out/std::format("{:03}-input.rgba16",frame),rgba.data(),rgba.size());
        c.begin();
        std::vector<unsigned> recorded(multiplier-1);
        c.transition(backbuffer.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        c.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,q.Count-2);
        for(unsigned generated=1;generated<multiplier;++generated){
        p.Set("DLSSG.Reset",frame==0||(fixture==L"recreate" && frame%8==0)?1u:0u);p.Set("DLSSG.BackbufferFrameID",static_cast<unsigned long long>(frame));
        p.Set("DLSSG.MultiFrameIndex",generated);
        p.stage=std::format("evaluate{}",frame);
        if(observe)FGObserver::Begin(c.list.Get(),queries.Get(),frame,generated);
        const unsigned base=(generated-1)*8194;
        c.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,base+8192);
        auto result=evaluate(c.list.Get(),feature,&p,nullptr);
        if(!NVSDK_NGX_SUCCEED(result))ngx(result,"Evaluate FG");
        c.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,base+8193);
        recorded[generated-1]=observe?FGObserver::End():0;
        c.transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readbacks[generated-1].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=layout;
        c.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        c.transition(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        c.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,q.Count-1);
        c.transition(backbuffer.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        c.list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,q.Count-2,2,times.Get(),UINT64(q.Count-2)*8);
        for(unsigned j=0;j<multiplier-1;++j){
            if(recorded[j])c.list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,j*8194,recorded[j],times.Get(),UINT64(j)*8194*8);
            c.list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,j*8194+8192,2,times.Get(),UINT64(j*8194+8192)*8);
        }
        c.finish();
        uint64_t* ts{};check(times->Map(0,nullptr,reinterpret_cast<void**>(&ts)));
        for(unsigned j=0;j<multiplier-1;++j)timing<<std::format("{},{},{:.9f},{}\n",frame,j+1,double(ts[j*8194+8193]-ts[j*8194+8192])*1000.0/double(c.frequency),recorded[j]);
        if(observe)FGObserver::Flush(ts,c.frequency);times->Unmap(0,nullptr);
        for(unsigned generated=1;generated<multiplier;++generated){
        auto& readback=readbacks[generated-1];
        void* pixels{};check(readback->Map(0,nullptr,&pixels));std::vector<char> packed(size_t(w)*h*8);
        for(unsigned y=0;y<h;++y)std::memcpy(packed.data()+size_t(y)*w*8,static_cast<char*>(pixels)+layout.Offset+size_t(y)*layout.Footprint.RowPitch,size_t(w)*8);
        readback->Unmap(0,nullptr);write(out/std::format("{:03}-{}.rgba16",frame,generated),packed.data(),packed.size());
        }
    }
    ngx(release(feature),"Release FG");ngx(shutdown(),"Provider shutdown");
    ngx(NVSDK_NGX_D3D12_DestroyParameters(raw),"Destroy parameters");
    ngx(NVSDK_NGX_D3D12_Shutdown1(c.device.Get()),"NGX shutdown");
    if(observe)FGObserver::Finish();
    std::ofstream(out/"complete.txt")<<"All requested evaluations and GPU waits completed; feature and NGX shutdown succeeded.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<std::endl;return 1;}
