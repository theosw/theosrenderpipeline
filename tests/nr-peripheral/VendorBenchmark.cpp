#include "GPU.h"
#include <numeric>
#include <nvsdk_ngx.h>

int wmain(int argc,wchar_t** argv){
    Require(argc==5,"usage: benchmark absolute-NR-DLL native|uniform|peripheral width height");
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    const std::wstring mode=argv[2];
    Require(mode==L"native" || mode==L"uniform" || mode==L"peripheral" || mode==L"bottleneck" || mode==L"cycle","benchmark mode");
    const auto w=static_cast<unsigned>(std::stoul(argv[3])),h=static_cast<unsigned>(std::stoul(argv[4]));
    Require(w>=64 && h>=64 && w<=5120 && h<=2880,"bounded benchmark dimensions");
    GPU gpu(true);
    // The real host initializes the normal NGX loader before constructing NR.
    const auto cache=std::filesystem::absolute("out/diagnostics/ngx-cache");std::filesystem::create_directories(cache);
    const auto init=NVSDK_NGX_D3D12_Init_with_ProjectID("f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "nr-peripheral-benchmark",cache.c_str(),gpu.device.Get(),nullptr,NVSDK_NGX_Version_API);
    Require(NVSDK_NGX_SUCCEED(init)||init==NVSDK_NGX_Result_FAIL_FeatureAlreadyExists,"normal NGX bootstrap");
    NeuralOptions options;options.enabled=true;options.runtimePath=argv[1];options.beforeUpscaling=true;
    options.tuning.uiCorrection=false;options.reconstruction.inputScale=mode==L"uniform"?.9f:1.f;
    options.reconstruction.peripheralCompression=mode==L"peripheral";
    options.reconstruction.bottleneckReuse=mode==L"bottleneck" || mode==L"cycle";
    std::vector<Pixel> pixels(size_t(w)*h);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
        pixels[y*w+x]={.1f+.6f*x/w,.1f+.6f*y/h,.2f+.3f*((x/8+y/8)%2),1};
    auto original=gpu.Texture(w,h,pixels,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto scene=gpu.Texture(w,h,{},DXGI_FORMAT_R8G8B8A8_UNORM);
    auto composed=gpu.Texture(w,h,pixels,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto ui=gpu.Texture(w,h,std::vector<Pixel>(size_t(w)*h,{}),DXGI_FORMAT_R8G8B8A8_UNORM);
    auto motion=gpu.Texture(w,h,std::vector<Pixel>(size_t(w)*h,{}),DXGI_FORMAT_R32G32_FLOAT);
    auto depth=gpu.Texture(w,h,std::vector<Pixel>(size_t(w)*h,{.5f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
    ComPtr<ID3D12QueryHeap> query;D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=2;
    Check(gpu.device->CreateQueryHeap(&q,IID_PPV_ARGS(&query)),"benchmark queries");
    auto timestamps=gpu.Buffer(16,D3D12_HEAP_TYPE_READBACK);UINT64 frequency{};
    Check(gpu.queue->GetTimestampFrequency(&frequency),"timestamp frequency");
    {
    auto pass=std::make_unique<NeuralPass>();std::vector<double> total,inference;
    for(unsigned frame=0;frame<50;++frame){
        const bool recreate=mode==L"cycle" && frame%10==0;
        if(recreate){
            options.beforeUpscaling=(frame/10)%2==0;
            options.passes=frame>=20?2:1;
            options.reconstruction.inputScale=frame>=20?.5f:1.f;
            options.reconstruction.peripheralCompression=frame>=30;
            options.reconstruction.fusedPreparation=frame>=30;
            options.reconstruction.bottleneckReuse=frame!=40;
            pass=std::make_unique<NeuralPass>(); // Last GPU.End completed every submission.
        }
        gpu.Begin();Check(Interop::RecordCopy(gpu.list.Get(),original.Get(),scene.Get()),"fresh scene");
        gpu.list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        const bool recorded=pass->Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,frame==0 || recreate,true,float(w),float(h),
            motion.Get(),depth.Get(),options.beforeUpscaling?nullptr:ui.Get(),scene.Get(),options.beforeUpscaling?nullptr:composed.Get(),frequency);
        Require(recorded,pass->Status().c_str());
        gpu.list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        gpu.list->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timestamps.Get(),0);gpu.End();
        pass->EvaluationSubmitted();
        Require(pass->BottleneckReused() == (options.reconstruction.bottleneckReuse && frame%2==1), "host pass reuse admission");
        pass->RetireTelemetry();
        if(frame>=10){
            void* data{};D3D12_RANGE range{0,16};Check(timestamps->Map(0,&range,&data),"query readback");
            auto* t=static_cast<UINT64*>(data);Require(t[1]>=t[0],"monotonic timestamps");total.push_back((t[1]-t[0])*1000.0/frequency);
            D3D12_RANGE noWrite{};timestamps->Unmap(0,&noWrite);
            const auto sample=pass->Telemetry().newestFirst[0];
            Require(sample.gpuFrequency==frequency,"inference clock domain");inference.push_back(sample.gpuTicks*1000.0/frequency);
        }
    }
    auto result=gpu.Read(pass->Corrected());size_t changed=0;
    for(size_t i=0;i<result.size();++i)for(unsigned ch=0;ch<4;++ch){
        Require(std::isfinite(result[i][ch]),"finite real NR output");
        if(ch<3 && std::abs(result[i][ch]-pixels[i][ch])>.01f)++changed;
    }
    Require(changed>result.size()/10,"real runtime must alter the image");
    auto mean=[](const auto& v){return std::accumulate(v.begin(),v.end(),0.0)/v.size();};
    std::printf("RESULT mode=%ls source=%ux%u model=%ux%u samples=%zu NR_total_ms=%.6f inference_ms=%.6f changed=%zu\n",mode.c_str(),w,h,
        TheosRenderPipeline::NeuralRendering::ModelExtent(w,options.reconstruction),TheosRenderPipeline::NeuralRendering::ModelExtent(h,options.reconstruction),
        total.size(),mean(total),mean(inference),changed);
    std::printf("STATE %s\n",pass->Status().c_str());
    for(size_t i=0;i<total.size();++i)std::printf("SAMPLE %zu %.6f %.6f\n",i,total[i],inference[i]);
    std::puts("CLEANUP begin: all frame work retired, releasing NeuralPass");
    std::fflush(stdout);
    } // All submissions retired before releasing the feature and normal loader.
    std::puts("CLEANUP feature released, shutting down normal NGX");std::fflush(stdout);
    Require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_Shutdown1(gpu.device.Get())),"normal NGX shutdown");
    std::puts("CLEANUP NGX shutdown complete");std::fflush(stdout);
}
