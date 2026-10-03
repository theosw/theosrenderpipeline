#include "AsyncPipeline.h"
#include "ReplayScene.h"
#include "FrameGen/NeuralRenderingFeatureSession.h"
#include <nvsdk_ngx.h>
#include <TlHelp32.h>
#include <atomic>
#include <chrono>
#include <thread>

using TRPExperiment::AsyncPipeline;
using Clock=std::chrono::steady_clock;
struct DeadlineTimer {
    HANDLE timer{CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS)};
    DeadlineTimer(){Require(timer!=nullptr,"high-resolution deadline timer");}
    ~DeadlineTimer(){CloseHandle(timer);}
    void WaitUntil(Clock::time_point deadline){
        const auto remaining=deadline-Clock::now();
        if(remaining<=Clock::duration::zero())return;
        LARGE_INTEGER due{};due.QuadPart=-std::max<std::int64_t>(1,std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count()/100);
        Require(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)!=FALSE,"deadline timer arm");
        Require(WaitForSingleObject(timer,1000)==WAIT_OBJECT_0,"deadline timer wait");
    }
};
static bool GameRunning(){
    const HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return true;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool running=false;
    if(!Process32FirstW(snapshot,&entry)){CloseHandle(snapshot);return true;}
    do{if(_wcsicmp(entry.szExeFile,L"SkyrimSE.exe")==0||_wcsicmp(entry.szExeFile,L"SkyrimVR.exe")==0)running=true;}while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return running;
}
struct GameGuard {
    std::atomic<bool> running{};std::jthread watcher;
    GameGuard():watcher([this](std::stop_token stop){
        while(!stop.stop_requested()){
            if(GameRunning()){running.store(true);return;}
            for(unsigned i=0;i<10&&!stop.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }){}
};
static void Shutdown(GPU& gpu,bool native){
    std::puts("CLEANUP feature released");std::fflush(stdout);
    if(native){
        std::puts("CLEANUP NGX shutdown begin");std::fflush(stdout);
        Require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_Shutdown1(gpu.device.Get())),"NGX shutdown");
    }
    std::puts("CLEANUP NGX shutdown complete");std::fflush(stdout);
}
static UINT64 LocalUsage(ID3D12Device* device){
    ComPtr<IDXGIFactory4> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"memory factory");
    ComPtr<IDXGIAdapter3> adapter;Check(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)),"memory adapter");
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};Check(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info),"local memory");
    return info.CurrentUsage;
}
int wmain(int argc,wchar_t** argv){
    Require(argc==9,"usage: replay absolute-NR-DLL off|regular|reset|async|init-only|create-only width height fps seconds output-directory capture-hz");
    Require(!GameRunning(),"Skyrim is running: defer native benchmark");
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    const std::wstring mode=argv[2];
    Require(mode==L"off"||mode==L"regular"||mode==L"reset"||mode==L"async"||mode==L"init-only"||mode==L"create-only","mode");
    const unsigned w=unsigned(std::stoul(argv[3])),h=unsigned(std::stoul(argv[4])),fps=unsigned(std::stoul(argv[5]));
    const unsigned seconds=unsigned(std::stoul(argv[6]));
    const std::wstring captureRequest=argv[8];size_t parsed{};
    const unsigned captureHz=unsigned(std::stoul(captureRequest,&parsed));
    const bool captureEvents=captureRequest.substr(parsed)==L"+events";
    Require(parsed==captureRequest.size()||captureEvents,"capture-hz suffix must be +events");
    Require(w>=64&&h>=64&&w<=2560&&h<=1440&&fps>=10&&fps<=120&&seconds>=1&&seconds<=12,"bounded replay parameters");
    Require(captureHz==0||(captureHz<=20&&fps%captureHz==0),"capture frequency must divide source fps, at most 20 Hz");
    Require(!captureEvents||captureHz>0,"dense event captures need a base capture rate");
    const auto output=std::filesystem::absolute(argv[7]);std::filesystem::create_directories(output);
    const auto runtime=std::filesystem::absolute(argv[1]);const auto cache=output/"ngx-cache";std::filesystem::create_directories(cache);
    GPU gpu(true);GameGuard guard;
    if(mode!=L"off"){
        const auto init=NVSDK_NGX_D3D12_Init_with_ProjectID("f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "nr-replay-standalone",cache.c_str(),gpu.device.Get(),nullptr,NVSDK_NGX_Version_API);
        Require(NVSDK_NGX_SUCCEED(init)||init==NVSDK_NGX_Result_FAIL_FeatureAlreadyExists,"normal NGX bootstrap");
    }
    if(mode==L"init-only"||mode==L"create-only"){
        if(mode==L"create-only"){
            TheosRenderPipeline::NeuralRendering::FeatureSession feature;
            TheosRenderPipeline::NeuralRendering::FeatureSession::CreateInfo create{};
            create.device=gpu.device.Get();create.runtimePath=runtime;create.allowReconstructionRuntime=true;
            create.displayWidth=create.renderWidth=w;create.displayHeight=create.renderHeight=h;
            const bool created=feature.EnsureInitialized(create);
            Require(created,feature.Status().c_str());
            std::puts("PHASE native feature created without evaluation");std::fflush(stdout);
        }
        std::printf("RESULT lifecycle=%ls evaluations=0\n",mode.c_str());std::fflush(stdout);Shutdown(gpu,true);return 0;
    }
    auto color=gpu.Texture(w,h,{},DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto motion=gpu.Texture(w,h,{},DXGI_FORMAT_R32G32_FLOAT);auto depth=gpu.Texture(w,h,{},DXGI_FORMAT_R32_FLOAT);
    ReplayScene scene(gpu.device.Get(),color.Get(),motion.Get(),depth.Get());
    auto pass=std::make_shared<NeuralPass>();NeuralOptions options;options.enabled=true;options.runtimePath=runtime;
    options.beforeUpscaling=true;options.tuning.uiCorrection=false;
    AsyncPipeline async;AsyncPipeline::Config config;config.width=w;config.height=h;config.enabled=mode==L"async";
    config.motionScaleX=float(w);config.motionScaleY=float(h);config.maxAge=16;
    if(mode==L"async")Check(async.Initialize(gpu.device.Get(),config,[pass,options,device=gpu.device,w,h](auto* list,const auto& capture){
        if(!pass->Record(device.Get(),list,0,options,true,true,float(w),float(h),capture.motion,capture.depth,nullptr,capture.color,nullptr))return false;
        return SUCCEEDED(Interop::RecordCopy(list,pass->Corrected(),capture.output));
    }),"async initialize");
    const unsigned frames=fps*seconds,warmup=16;
    std::vector<unsigned> captureFrames;
    if(captureHz)for(unsigned frame=0;frame<frames;++frame){
        bool take=frame%(fps/captureHz)==0;
        if(captureEvents)for(double event:{0.,1.5,3.,4.5,5.1}){
            const int eventFrame=int(std::floor(event*fps));
            take=take||(int(frame)>=eventFrame-2&&int(frame)<=eventFrame+7);
        }
        if(take)captureFrames.push_back(frame);
    }
    std::vector<ReplayCapture> captures;
    if(captureHz){
        const auto desc=color->GetDesc();UINT64 bytes{};
        gpu.device->GetCopyableFootprints(&desc,0,1,0,nullptr,nullptr,nullptr,&bytes);
        Require(bytes*captureFrames.size()<=512ull*1024*1024,"capture buffers exceed the separate 512 MiB bound; lower extent or capture rate");
        for(unsigned i=0;i<captureFrames.size();++i)captures.emplace_back(gpu,color.Get());
    }
    std::ofstream samples(output/"samples.csv"),images(output/"captures.csv");Require(bool(samples)&&bool(images),"CSV files");
    samples<<"frame,scene,source_time,reset,cpu_frame_ms,cpu_record_ms,cpu_submit_wait_ms,host_gpu_ms,producer_interval_ms,start_lateness_ms,deadline_miss,evaluations,result_age,result_age_ms,result_wall_age_ms,result_frame,displaying,evaluation_roundtrip_ms,dropped\n";
    images<<"frame,scene,source_time,file\n";
    ComPtr<ID3D12QueryHeap> query;D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=2;
    Check(gpu.device->CreateQueryHeap(&q,IID_PPV_ARGS(&query)),"host timestamps");auto timestamp=gpu.Buffer(16,D3D12_HEAP_TYPE_READBACK);
    UINT64 frequency{};Check(gpu.queue->GetTimestampFrequency(&frequency),"timestamp frequency");
    DeadlineTimer timer;
    const auto period=std::chrono::duration<double>(1.0/fps);Clock::time_point epoch{},previousBegin{};
    std::vector<Clock::time_point> sourceBegins(warmup+frames);
    unsigned previousScene=0,captureIndex=0;std::uint64_t synchronousEvaluations=0,nativeResets=0;UINT64 memoryWarm{};
    for(unsigned sequence=0;sequence<warmup+frames;++sequence){
        Require(!guard.running.load(),"game started during benchmark: stop further frames");
        const bool measured=sequence>=warmup;const unsigned frame=measured?sequence-warmup:0;
        const float absoluteTime=measured?float(frame)/fps:0.f;
        const unsigned sceneIndex=std::min(unsigned(absoluteTime/1.5f),3u);
        const bool reset=sequence==0||sceneIndex!=previousScene;
        const float time=absoluteTime-sceneIndex*1.5f;
        const float previous=reset?time:(measured?std::max(0.f,time-1.f/fps):0.f);
        if(sequence==warmup){memoryWarm=LocalUsage(gpu.device.Get());epoch=Clock::now();}
        const auto deadline=epoch+std::chrono::duration_cast<Clock::duration>(period*frame);
        if(measured)timer.WaitUntil(deadline);
        const auto begin=Clock::now();const double interval=measured&&frame?std::chrono::duration<double,std::milli>(begin-previousBegin).count():0;
        sourceBegins[sequence]=begin;
        const double lateness=measured?std::chrono::duration<double,std::milli>(begin-deadline).count():0;
        gpu.Begin();gpu.list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        scene.Record(gpu,color.Get(),motion.Get(),depth.Get(),time,previous,sceneIndex);
        ID3D12Resource* result=color.Get();
        if(mode==L"regular"||mode==L"reset"){
            const bool nativeReset=reset||mode==L"reset";nativeResets+=nativeReset;
            const bool recorded=pass->Record(gpu.device.Get(),gpu.list.Get(),0,options,nativeReset,true,float(w),float(h),motion.Get(),depth.Get(),nullptr,color.Get(),nullptr);
            Require(recorded,pass->Status().c_str());
            result=pass->Corrected();++synchronousEvaluations;
        }else if(mode==L"async"){
            const auto recorded=async.Record(gpu.list.Get(),color.Get(),motion.Get(),depth.Get(),reset);
            Check(recorded,"async record");Require(recorded==S_OK,"retired host frame admitted exactly once");result=async.Output();
        }
        gpu.list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        gpu.list->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timestamp.Get(),0);
        const bool capture=measured&&captureIndex<captureFrames.size()&&frame==captureFrames[captureIndex];
        if(capture)captures[captureIndex].Record(gpu,result);
        Check(gpu.list->Close(),"host close");const auto recordedAt=Clock::now();
        ID3D12CommandList* lists[]{gpu.list.Get()};gpu.queue->ExecuteCommandLists(1,lists);
        Check(gpu.queue->Signal(gpu.fence.Get(),++gpu.serial),"host signal");
        if(mode==L"async")Check(async.Submitted(gpu.fence.Get(),gpu.serial),"async submitted");
        Check(gpu.fence->SetEventOnCompletion(gpu.serial,gpu.event),"host completion");
        Require(WaitForSingleObject(gpu.event,10000)==WAIT_OBJECT_0&&gpu.fence->GetCompletedValue()==gpu.serial,"host retirement");
        const auto end=Clock::now();const double cpu=std::chrono::duration<double,std::milli>(end-begin).count();
        void* data{};D3D12_RANGE range{0,16};Check(timestamp->Map(0,&range,&data),"timestamps map");
        const auto* t=static_cast<UINT64*>(data);const auto ticks=t[1]-t[0];D3D12_RANGE none{};timestamp->Unmap(0,&none);
        const auto status=async.Status();Require(!status.failed,status.error.c_str());
        Require(status.resultFrame<=sequence+1,"result source frame belongs to this replay");
        const auto wallAge=status.displaying?std::chrono::duration<double,std::milli>(end-sourceBegins[status.resultFrame-1]).count():0;
        if(measured){
            const bool missed=end>deadline+std::chrono::duration_cast<Clock::duration>(period);
            samples<<frame<<','<<sceneIndex<<','<<absoluteTime<<','<<reset<<','<<cpu<<','
                <<std::chrono::duration<double,std::milli>(recordedAt-begin).count()<<','<<std::chrono::duration<double,std::milli>(end-recordedAt).count()<<','
                <<ticks*1000.0/frequency<<','<<interval<<','<<lateness<<','<<missed<<','
                <<(mode==L"async"?status.evaluations:synchronousEvaluations)<<','<<status.resultAge<<','<<status.resultAge*1000.0/fps<<','<<wallAge<<','<<status.resultFrame<<','
                <<status.displaying<<','<<status.evaluationMs<<','<<status.dropped<<'\n';
            if(capture){images<<frame<<','<<sceneIndex<<','<<absoluteTime<<",frame-"<<frame<<".rgba16f\n";++captureIndex;}
            previousBegin=begin;
        }
        previousScene=sceneIndex;
        if(mode==L"async"&&sequence==0){
            const auto until=Clock::now()+std::chrono::seconds(30);
            while(!async.Status().evaluations&&!async.Status().failed&&Clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Require(async.Status().evaluations&&!async.Status().failed,"initial async evaluation completed");
        }
    }
    samples.flush();images.flush();Require(bool(samples)&&bool(images),"flush replay CSVs");
    const auto status=async.Status();const auto memoryEnd=LocalUsage(gpu.device.Get());
    // Stop after the final host fence, before mapping captures; preserve failures.
    Require(async.Stop(),"host and worker retirement");
    if(mode==L"async")Require(status.evaluations>0,"native async evaluation activity");
    for(unsigned i=0;i<captures.size();++i)captures[i].Write(output/("frame-"+std::to_string(captureFrames[i])+".rgba16f"));
    std::printf("RESULT mode=%ls size=%ux%u fps=%u frames=%u captures=%u evaluations=%llu native_resets=%llu allocation=%llu local_usage_warm=%llu local_usage_end=%llu\n",
        mode.c_str(),w,h,fps,frames,captureIndex,mode==L"async"?status.evaluations:synchronousEvaluations,
        mode==L"async"?status.evaluations:nativeResets,status.allocationBytes,memoryWarm,memoryEnd);
    std::fflush(stdout);pass.reset();Shutdown(gpu,mode!=L"off");
}
