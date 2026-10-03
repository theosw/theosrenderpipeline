#include "GPU.h"
#include "FrameGen/SourceDLSSGNeuralExecution.h"
#include <nvsdk_ngx.h>
#include <TlHelp32.h>
#include <thread>
#include <chrono>
bool GameRunning()
{
    auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return true;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool running=false;
    if(!Process32FirstW(snapshot,&entry)){CloseHandle(snapshot);return true;}
    do { running|=_wcsicmp(entry.szExeFile,L"SkyrimSE.exe")==0; } while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return running;
}
int wmain(int argc,wchar_t** argv)
{
    Require(argc==5,"usage: integration absolute-NR-DLL width height cache-path");
    Require(!GameRunning(),"defer integration while Skyrim is running");
    GPU gpu(true);const UINT w=std::stoul(argv[2]),h=std::stoul(argv[3]);
    Require(w>=64&&w<=5120&&h>=64&&h<=1440,"bounded integration extent");
    auto init=NVSDK_NGX_D3D12_Init_with_ProjectID("f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "nr-integration",argv[4],gpu.device.Get(),nullptr,NVSDK_NGX_Version_API);
    Require(NVSDK_NGX_SUCCEED(init)||init==NVSDK_NGX_Result_FAIL_FeatureAlreadyExists,"NGX bootstrap");
    auto color=gpu.Texture(w,h,std::vector<Pixel>(size_t(w)*h,{.4f,.35f,.3f,.37f}),DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto motion=gpu.Texture(w/2,h/2,std::vector<Pixel>(size_t(w/2)*(h/2),{}),DXGI_FORMAT_R16G16_FLOAT);
    auto depth=gpu.Texture(w/2,h/2,std::vector<Pixel>(size_t(w/2)*(h/2),{.5f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
    NeuralOptions options;options.enabled=true;options.beforeUpscaling=true;options.runtimePath=std::filesystem::absolute(argv[1]);
    options.reconstruction.inputScale=.5f;
    std::unique_ptr<NeuralExecution> execution;
    // Live transitions use the same recreation predicate and retirement entry
    // points as Backend. A fresh worker receives per-capture options, two passes
    // and native RG16 guides. No benchmark-only evaluator replaces the runtime.
    for(unsigned phase=0;phase<5;++phase) {
        options.async=phase>=1&&phase<=3;options.passes=phase==2||phase==3?2:1;
        if(phase==3)color=gpu.Texture(w*3/4,h*3/4,std::vector<Pixel>(size_t(w*3/4)*(h*3/4),{.4f,.35f,.3f,.37f}),DXGI_FORMAT_R16G16B16A16_FLOAT);
        options.secondPass.linked=false;options.secondPass.inputScale=.25f;
        if(execution) {
            Require(execution->NeedsRecreation(options,w/2,h/2,color.Get(),motion->GetDesc().Format),"live switch requests retirement");
            Require(execution->RetireAsync(),"retire worker before mode/pass switch");execution.reset();
        }
        execution=std::make_unique<NeuralExecution>();bool displayed=false;
        for(unsigned frame=0;frame<1000;++frame) {
            Require(!GameRunning(),"game started: stop native integration");
            gpu.Begin();
            Require(execution->Record(gpu.device.Get(),gpu.list.Get(),0,options,frame==0,true,float(w/2),float(h/2),
                motion.Get(),depth.Get(),nullptr,color.Get(),nullptr),execution->Status().c_str());
            gpu.End();Check(execution->Submitted(gpu.fence.Get(),gpu.serial),"publish host completion");
            displayed|=execution->Status().find("delayed correction")!=std::string::npos;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if(frame>=23 && (!options.async || execution->Status().find("delayed correction")!=std::string::npos))break;
        }
        if(options.async)Require(displayed,"async actually produced a delayed correction");
        if(options.async) {
            // Exercise the production wrapper with three submitted host lists
            // genuinely in flight, rather than draining every host frame first.
            std::array<ComPtr<ID3D12CommandAllocator>,3> allocators;
            std::array<ComPtr<ID3D12GraphicsCommandList>,3> lists;
            ComPtr<ID3D12Fence> gate;
            Check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"native queued host gate");
            for(unsigned i=0;i<lists.size();++i) {
                Check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[i])),"native queued allocator");
                Check(gpu.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[i].Get(),nullptr,IID_PPV_ARGS(&lists[i])),"native queued list");
            }
            Check(gpu.queue->Wait(gate.Get(),1),"native hold host queue");
            for(unsigned i=0;i<lists.size();++i) {
                const bool ok=execution->Record(gpu.device.Get(),lists[i].Get(),i,options,false,true,float(w/2),float(h/2),
                    motion.Get(),depth.Get(),nullptr,color.Get(),nullptr);
                if(!ok)Check(gate->Signal(1),"failed native gate release");
                Require(ok,execution->Status().c_str());
                Check(lists[i]->Close(),"native queued close");ID3D12CommandList* submitted[]{lists[i].Get()};gpu.queue->ExecuteCommandLists(1,submitted);
                Check(gpu.queue->Signal(gpu.fence.Get(),++gpu.serial),"native queued signal");
                Check(execution->Submitted(gpu.fence.Get(),gpu.serial),"native queued publication");
                Require(gpu.fence->GetCompletedValue()<gpu.serial,"native host really in flight");
                // The last existing result can expire legitimately; the wrapper
                // must still advance/record every frame without retirement bypass.
                Require(execution->Corrected()!=color.Get() && execution->Status().find("capture retires")==std::string::npos,"native queued frame never bypassed");
            }
            Check(gate->Signal(1),"native release host queue");
            Check(gpu.fence->SetEventOnCompletion(gpu.serial,gpu.event),"native queued completion event");
            Require(WaitForSingleObject(gpu.event,10000)==WAIT_OBJECT_0,"native queued host retired");
            std::puts("PASS three native host frames recorded/submitted while retirement was gated");
        }
        auto pixels=gpu.Read(execution->Corrected());
        for(const auto& p:pixels) {Require(std::isfinite(p[0])&&std::isfinite(p[1])&&std::isfinite(p[2]),"finite corrected pixels");Near(p[3],.37,1e-3,"current alpha");}
        std::printf("PASS phase=%u async=%u passes=%d %s\n",phase,options.async,options.passes,execution->Status().c_str());
    }
    Require(execution->RetireAsync(),"final worker retirement");execution.reset();
    Require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_Shutdown1(gpu.device.Get())),"NGX shutdown");
    std::puts("PASS production execution, queued host frames, regular/async/2-pass/scene-resize/regular switches and shutdown");
}
