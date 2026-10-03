#include "AsyncPipeline.h"
#include "GPU.h"
#include <nvsdk_ngx.h>
#include <TlHelp32.h>
#include <chrono>
#include <atomic>
#include <fstream>
#include <numeric>
#include <thread>

using TRPExperiment::AsyncPipeline;
static bool GameRunning() {
    const HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return true;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool running=false;
    if(!Process32FirstW(snapshot,&entry)){CloseHandle(snapshot);return true;}
    do {if(_wcsicmp(entry.szExeFile,L"SkyrimSE.exe")==0 || _wcsicmp(entry.szExeFile,L"SkyrimVR.exe")==0)running=true;}while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return running;
}
// Process inventory can take several milliseconds. Keep it off the producer
// path, otherwise even a nominally unpaced benchmark gives NR time to finish
// between every frame. The same guard runs in all modes.
struct GameGuard {
    std::atomic<bool> running{};
    std::jthread watcher;
    GameGuard():watcher([this](std::stop_token stop) {
        while(!stop.stop_requested()) {
            if(GameRunning()){running.store(true);return;}
            for(unsigned i=0;i<10 && !stop.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }){}
};
// Same moving procedural scene and guide convention for both modes. Setup and
// PNG/raw readback are outside the frame timer; shader execution is included.
struct Scene {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;ComPtr<ID3D12DescriptorHeap> heap;
    explicit Scene(ID3D12Device* d,ID3D12Resource* color,ID3D12Resource* motion,ID3D12Resource* depth):device(d) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,0};
        D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,3};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={1,&range};
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"scene root serialize");
        Check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"scene root");
        const char shader[]=R"(RWTexture2D<float4> Color:register(u0);RWTexture2D<float2> Motion:register(u1);RWTexture2D<float> Depth:register(u2);
        cbuffer Params:register(b0){uint Width,Height,Frame;}
        [numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){if(p.x>=Width||p.y>=Height)return;
        float x=p.x-Frame*.5;float checker=fmod(floor(x/12)+floor(p.y/12),2)!=0?.1:0;
        float3 c=float3(.2+.25*p.x/Width,.2+.25*p.y/Height,.25)+checker;
        Color[p.xy]=float4(c,1);Motion[p.xy]=float2(-.5/Width,0);Depth[p.xy]=.5;})";
        Check(D3DCompile(shader,sizeof(shader)-1,"async-moving-scene",nullptr,nullptr,"main","cs_5_1",0,0,&blob,&error),"scene shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
        Check(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"scene pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        Check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"scene descriptors");
        auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
        for(auto* resource:{color,motion,depth}) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=resource->GetDesc().Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            d->CreateUnorderedAccessView(resource,nullptr,&uav,cpu);cpu.ptr+=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    void Record(GPU& gpu,ID3D12Resource* color,ID3D12Resource* motion,ID3D12Resource* depth,unsigned frame,unsigned w,unsigned h) {
        for(auto* r:{color,motion,depth})gpu.Barrier(r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto* hd=heap.Get();gpu.list->SetDescriptorHeaps(1,&hd);gpu.list->SetComputeRootSignature(root.Get());gpu.list->SetPipelineState(pipeline.Get());
        unsigned constants[]{w,h,frame};gpu.list->SetComputeRoot32BitConstants(0,3,constants,0);
        gpu.list->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());gpu.list->Dispatch((w+7)/8,(h+7)/8,1);
        for(auto* r:{color,motion,depth})gpu.Barrier(r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
    }
};
int wmain(int argc,wchar_t** argv) {
    Require(argc==8,"usage: benchmark absolute-NR-DLL async|sync|off width height frames output-directory frame-pause-microseconds");
    Require(!GameRunning(),"Skyrim is running: defer native benchmark until the GPU is idle");
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    const std::wstring mode=argv[2];Require(mode==L"async" || mode==L"sync" || mode==L"off","mode");
    const unsigned w=unsigned(std::stoul(argv[3])),h=unsigned(std::stoul(argv[4])),frames=unsigned(std::stoul(argv[5]));
    const unsigned pause=unsigned(std::stoul(argv[7]));Require(pause<=100000,"bounded frame pause");
    Require(w>=64 && h>=64 && w<=5120 && h<=2880 && frames>=32 && frames<=5000,"bounded parameters");
    const auto outputDir=std::filesystem::absolute(argv[6]);std::filesystem::create_directories(outputDir);
    std::ofstream samples(outputDir/"samples.csv");Require(bool(samples),"CSV output");
    samples<<"frame,cpu_frame_ms,host_gpu_ms,evaluations,result_age,result_frame,displaying,evaluation_roundtrip_ms\n";
    GPU gpu(true);
    auto original=gpu.Texture(w,h,{},DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto motion=gpu.Texture(w,h,{},DXGI_FORMAT_R32G32_FLOAT);
    auto depth=gpu.Texture(w,h,{},DXGI_FORMAT_R32_FLOAT);
    Scene scene(gpu.device.Get(),original.Get(),motion.Get(),depth.Get());
    const auto cache=outputDir/"ngx-cache";std::filesystem::create_directories(cache);
    if(mode!=L"off") {
        const auto init=NVSDK_NGX_D3D12_Init_with_ProjectID("f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "nr-async-standalone",cache.c_str(),gpu.device.Get(),nullptr,NVSDK_NGX_Version_API);
        Require(NVSDK_NGX_SUCCEED(init)||init==NVSDK_NGX_Result_FAIL_FeatureAlreadyExists,"normal NGX bootstrap");
    }
    auto pass=std::make_shared<NeuralPass>();NeuralOptions options;options.enabled=true;options.runtimePath=std::filesystem::absolute(argv[1]);
    options.beforeUpscaling=true;options.tuning.uiCorrection=false;
    AsyncPipeline async;AsyncPipeline::Config config;config.width=w;config.height=h;config.enabled=mode==L"async";
    config.motionScaleX=float(w);config.motionScaleY=float(h);config.maxAge=16;
    if(mode==L"async") {
        // Shared ownership keeps the feature alive even if the pipeline must
        // retain an incomplete inference submission on a terminal failure.
        Check(async.Initialize(gpu.device.Get(),config,[pass,options,device=gpu.device,w,h](auto* list,const auto& capture) {
            if(!pass->Record(device.Get(),list,0,options,true,true,float(w),float(h),capture.motion,capture.depth,nullptr,capture.color,nullptr))return false;
            return SUCCEEDED(Interop::RecordCopy(list,pass->Corrected(),capture.output));
        }),"async initialize");
    }
    ComPtr<ID3D12QueryHeap> query;D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=2;
    Check(gpu.device->CreateQueryHeap(&q,IID_PPV_ARGS(&query)),"host timestamps");auto readback=gpu.Buffer(16,D3D12_HEAP_TYPE_READBACK);
    UINT64 frequency{};Check(gpu.queue->GetTimestampFrequency(&frequency),"host timestamp frequency");
    std::vector<double> durations;ID3D12Resource* result=original.Get();
    GameGuard guard;
    for(unsigned frame=0;frame<frames;++frame) {
        Require(!guard.running.load(),"game started during benchmark: stop before further frames");
        const auto begin=std::chrono::steady_clock::now();gpu.Begin();
        gpu.list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);scene.Record(gpu,original.Get(),motion.Get(),depth.Get(),frame,w,h);
        if(mode==L"sync") {
            Require(pass->Record(gpu.device.Get(),gpu.list.Get(),0,options,true,true,float(w),float(h),motion.Get(),depth.Get(),nullptr,original.Get(),nullptr),pass->Status().c_str());
            result=pass->Corrected();
        }else if(mode==L"async") {
            Check(async.Record(gpu.list.Get(),original.Get(),motion.Get(),depth.Get(),frame==0),"async record");result=async.Output();
        }
        gpu.list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);gpu.list->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,readback.Get(),0);
        Check(gpu.list->Close(),"host close");ID3D12CommandList* lists[]{gpu.list.Get()};gpu.queue->ExecuteCommandLists(1,lists);
        Check(gpu.queue->Signal(gpu.fence.Get(),++gpu.serial),"host signal");
        if(mode==L"async")Check(async.Submitted(gpu.fence.Get(),gpu.serial),"async submitted");
        Check(gpu.fence->SetEventOnCompletion(gpu.serial,gpu.event),"host event");Require(WaitForSingleObject(gpu.event,10000)==WAIT_OBJECT_0,"host frame retirement");
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        if(frame>=16)durations.push_back(ms);
        void* data{};D3D12_RANGE range{0,16};Check(readback->Map(0,&range,&data),"timestamps map");auto* t=static_cast<UINT64*>(data);
        const auto ticks=t[1]-t[0];D3D12_RANGE none{};readback->Unmap(0,&none);
        auto status=async.Status();Require(!status.failed,status.error.c_str());
        samples<<frame<<','<<ms<<','<<ticks*1000.0/frequency<<','<<(mode==L"sync"?frame+1:status.evaluations)<<','<<status.resultAge<<','
            <<status.resultFrame<<','<<status.displaying<<','<<status.evaluationMs<<'\n';
        if(mode==L"async" && frame==0) {
            // Initial runtime/feature setup is outside the measured population.
            // Otherwise a fast host can finish the experiment before NR starts.
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            while(!async.Status().evaluations && !async.Status().failed && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Require(async.Status().evaluations && !async.Status().failed,"initial native async evaluation completed");
        }
        // Fixed opportunity to progress, outside the measured frame scope, in
        // both modes. This is a procedural harness, not a game/FPS benchmark.
        if(pause)std::this_thread::sleep_for(std::chrono::microseconds(pause));
    }
    samples.flush();Require(bool(samples),"flush timing samples before vendor shutdown");
    auto pixels=gpu.Read(result);std::ofstream raw(outputDir/"final-rgba32f.bin",std::ios::binary);
    raw.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()*sizeof(Pixel)));
    raw.flush();Require(bool(raw),"flush image before vendor shutdown");
    for(const auto& pixel:pixels)for(float channel:pixel)Require(std::isfinite(channel),"finite output");
    auto status=async.Status();
    if(mode==L"async")Require(status.evaluations && status.resultFrame,"actual NR results consumed");
    std::sort(durations.begin(),durations.end());const auto mean=std::accumulate(durations.begin(),durations.end(),0.0)/durations.size();
    auto percentile=[&](double p){return durations[std::min(size_t(p*(durations.size()-1)),durations.size()-1)];};
    std::printf("RESULT mode=%ls size=%ux%u samples=%zu cpu_mean=%.6f p50=%.6f p95=%.6f p99=%.6f evaluations=%llu age=%u allocated=%llu\n",
        mode.c_str(),w,h,durations.size(),mean,percentile(.5),percentile(.95),percentile(.99),mode==L"sync"?std::uint64_t(frames):status.evaluations,status.resultAge,status.allocationBytes);
    std::fflush(stdout);Require(async.Stop(),"async host and worker retirement");
    pass.reset();std::puts("CLEANUP feature released");std::fflush(stdout);
    if(mode!=L"off")Require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_Shutdown1(gpu.device.Get())),"normal NGX shutdown");
    std::puts("CLEANUP NGX shutdown complete");
}
