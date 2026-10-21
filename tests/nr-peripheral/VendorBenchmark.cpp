#include "GPU.h"
#include "KernelProfiler.h"
#include <map>
#include <numeric>
#include <fstream>
#include <nvsdk_ngx.h>

int wmain(int argc,wchar_t** argv){
    Require(argc >= 5 && argc <= 7, "usage: benchmark absolute-NR-DLL native|uniform|peripheral|passes|coldpasses width height [preset [output-float4-file]]");
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    const std::wstring mode=argv[2];
    const bool coldStart = mode == L"coldpasses";
    const bool twoPassCases = mode == L"passes" || coldStart;
    Require(mode == L"native" || mode == L"uniform" || mode == L"peripheral" || twoPassCases, "benchmark mode");
    const auto w=static_cast<unsigned>(std::stoul(argv[3])),h=static_cast<unsigned>(std::stoul(argv[4]));
    const bool profileKernels=KernelProfiler::Install();
    struct KernelTotal{double ms{},maxMs{};unsigned count{};std::uint32_t grid[3]{},block[3]{},shared{};};
    std::map<std::string,KernelTotal> kernelTotals;unsigned profiledFrames=0;
    Require(w>=64 && h>=64 && w<=5120 && h<=2880,"bounded benchmark dimensions");
    GPU gpu(true);
    // The legacy benchmark supplies a warm loader. coldpasses exercises the
    // production NR bootstrap without a prior DLSS/Streamline initialization.
    const auto cache=std::filesystem::absolute("out/diagnostics/ngx-cache");std::filesystem::create_directories(cache);
    bool bootstrapInitialized=false;
    if (coldStart) {
        Require(!GetModuleHandleW(L"nvngx.dll") && !GetModuleHandleW(L"_nvngx.dll"),
            "cold NR starts without normal NGX loader");
        Require(!GetModuleHandleW(L"sl.interposer.dll") && !GetModuleHandleW(L"nvngx_dlss.dll"),
            "cold NR starts without Streamline or DLSS");
    } else {
        const auto init =
            NVSDK_NGX_D3D12_Init_with_ProjectID("f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                "nr-peripheral-benchmark", cache.c_str(), gpu.device.Get(), nullptr, NVSDK_NGX_Version_API);
        bootstrapInitialized=NVSDK_NGX_SUCCEED(init) || init==NVSDK_NGX_Result_FAIL_FeatureAlreadyExists;
        // TRP_NR_PASCAL_RESEARCH=1: the driver's NGX core refuses GTX 10 (FeatureNotSupported);
        // the NR session then finds the loader itself.
        wchar_t research[4]{};
        const bool pascalResearch = GetEnvironmentVariableW(L"TRP_NR_PASCAL_RESEARCH", research, 4) == 1 && research[0] == L'1';
        if (!NVSDK_NGX_SUCCEED(init)) { std::printf("normal NGX bootstrap result=0x%08X\n", static_cast<unsigned>(init)); }
        Require(NVSDK_NGX_SUCCEED(init) || init == NVSDK_NGX_Result_FAIL_FeatureAlreadyExists ||
                (pascalResearch && init == NVSDK_NGX_Result_FAIL_FeatureNotSupported), "normal NGX bootstrap");
    }
    NeuralOptions options;options.enabled=true;options.runtimePath=argv[1];options.beforeUpscaling=true;
    options.tuning.uiCorrection=false;options.reconstruction.inputScale=mode==L"uniform"?.9f:1.f;
    options.reconstruction.peripheralCompression=mode==L"peripheral";
    if(argc>=6)options.reconstruction.preset=std::stoi(argv[5]);
    std::vector<Pixel> pixels(size_t(w)*h);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
        pixels[y*w+x]={.1f+.6f*x/w,.1f+.6f*y/h,.2f+.3f*((x/8+y/8)%2),1};
    const auto format = twoPassCases ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    auto original=gpu.Texture(w,h,pixels,format);
    auto scene=gpu.Texture(w,h,{},format);
    auto composed=gpu.Texture(w,h,pixels,format);
    auto ui=gpu.Texture(w,h,std::vector<Pixel>(size_t(w)*h,{}),format);
    auto motion=gpu.Texture(w,h,std::vector<Pixel>(size_t(w)*h,{}),DXGI_FORMAT_R32G32_FLOAT);
    auto depth=gpu.Texture(w,h,std::vector<Pixel>(size_t(w)*h,{.5f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
    ComPtr<ID3D12QueryHeap> query;D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=2;
    Check(gpu.device->CreateQueryHeap(&q,IID_PPV_ARGS(&query)),"benchmark queries");
    auto timestamps=gpu.Buffer(16,D3D12_HEAP_TYPE_READBACK);UINT64 frequency{};
    Check(gpu.queue->GetTimestampFrequency(&frequency),"timestamp frequency");
    {
    auto pass=std::make_unique<NeuralPass>();std::vector<double> total,inference;
    for(unsigned frame=0;frame<50;++frame){
        const bool recreate = twoPassCases && frame % 10 == 0;
        if(recreate){
            options.beforeUpscaling=(frame/10)%2==0;
            options.passes=2;options.reconstruction.inputScale=frame==20?.5f:.75f;
            options.reconstruction.peripheralCompression=frame>=20;
            options.reconstruction.fusedPreparation=frame>=30;
            options.extraPasses[0].linked=frame==0;
            options.extraPasses[0].inputScale=frame==10 || frame==30?.25f:1.f;
            options.extraPasses[0].preset=frame>=30?1:0;
            options.extraPasses[0].tuning.intensity=.7f;
            options.tuning.uiCorrection=!options.beforeUpscaling;
            options.extraPasses[0].tuning.uiCorrection=!options.beforeUpscaling;
            pass=std::make_unique<NeuralPass>(); // Last GPU.End completed every submission.
        }
        gpu.Begin();Check(Interop::RecordCopy(gpu.list.Get(),original.Get(),scene.Get()),"fresh scene");
        if(profileKernels && frame>=10)KernelProfiler::BeginFrame();
        gpu.list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        const bool recorded=pass->Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,frame==0 || recreate,true,float(w),float(h),
            motion.Get(),depth.Get(),options.beforeUpscaling?nullptr:ui.Get(),scene.Get(),options.beforeUpscaling?nullptr:composed.Get(),frequency);
        if(!recorded && pass->RefusedCreation())std::printf("REFUSED 0x%08X\n",pass->RefusedCreation());
        if (!recorded && pass->Status().find("NR launch rejected") != std::string::npos) {
            const auto reason=pass->Status();
            gpu.Discard(); // Earlier kernels may depend on this dropped chain.
            Check(gpu.device->GetDeviceRemovedReason(),"device healthy after refused NR launch");
            pass.reset();
            std::printf("NR_OFF reason=%s device_removed=0x00000000 submitted=none frame=none clean_exit=1\n",reason.c_str());
            return 0;
        }
        if (!recorded) std::printf("REFUSED_BEFORE_SUBMISSION device_removed=0x%08lX\n",static_cast<unsigned long>(gpu.device->GetDeviceRemovedReason()));
        Require(recorded,pass->Status().c_str());
        if (coldStart) {
            Require(GetModuleHandleW(L"nvngx.dll") || GetModuleHandleW(L"_nvngx.dll"),
                "production NR established normal NGX loader");
            Require(!GetModuleHandleW(L"sl.interposer.dll") && !GetModuleHandleW(L"nvngx_dlss.dll"),
                "NR bootstrap remains independent of Streamline and DLSS");
        }
        gpu.list->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        gpu.list->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timestamps.Get(),0);gpu.End();
        if(profileKernels && frame>=10){
            ++profiledFrames;
            for(const auto& s:KernelProfiler::EndFrame(frequency)){
                auto& t=kernelTotals[s.name];t.ms+=s.ms;t.maxMs=(std::max)(t.maxMs,s.ms);++t.count;
                std::copy_n(s.grid,3,t.grid);std::copy_n(s.block,3,t.block);t.shared=s.sharedBytes;
            }
        }
        if(recreate)std::printf("CASE frame=%u %s\n",frame,pass->Status().c_str());
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
    if(argc==7){
        std::ofstream output(std::filesystem::path(argv[6]),std::ios::binary);
        output.write(reinterpret_cast<const char*>(result.data()),static_cast<std::streamsize>(result.size()*sizeof(Pixel)));
        Require(output.good(),"write complete float4 output for runtime comparison");
    }
    for(size_t i=0;i<result.size();++i)for(unsigned ch=0;ch<4;++ch){
        Require(std::isfinite(result[i][ch]),"finite real NR output");
        if(ch<3 && std::abs(result[i][ch]-pixels[i][ch])>.01f)++changed;
    }
    Require(changed>result.size()/10,"real runtime must alter the image");
    // A runtime without working kernels for this GPU can run and still render
    // black (community int8 v2 on RTX 40), which also "alters the image".
    double inputMean=0,outputMean=0;size_t black=0;
    for(size_t i=0;i<result.size();++i){
        inputMean+=(pixels[i][0]+pixels[i][1]+pixels[i][2])/3.0;outputMean+=(result[i][0]+result[i][1]+result[i][2])/3.0;
        if(result[i][0]<.02f && result[i][1]<.02f && result[i][2]<.02f)++black;
    }
    std::uint64_t fingerprint=1469598103934665603ull;
    for(const auto& p:result)for(unsigned ch=0;ch<3;++ch){fingerprint^=static_cast<std::uint64_t>(std::llround(p[ch]*1000.0));fingerprint*=1099511628211ull;}
    std::printf("OUTPUT input_mean=%.4f output_mean=%.4f black_share=%.4f fingerprint=%016llX\n",inputMean/result.size(),outputMean/result.size(),double(black)/result.size(),static_cast<unsigned long long>(fingerprint));
    auto mean=[](const auto& v){return std::accumulate(v.begin(),v.end(),0.0)/v.size();};
    std::printf("RESULT mode=%ls source=%ux%u model=%ux%u samples=%zu NR_total_ms=%.6f inference_ms=%.6f changed=%zu\n",mode.c_str(),w,h,
        TheosRenderPipeline::NeuralRendering::ModelExtent(w,options.reconstruction),TheosRenderPipeline::NeuralRendering::ModelExtent(h,options.reconstruction),
        total.size(),mean(total),mean(inference),changed);
    std::printf("STATE %s\n",pass->Status().c_str());
    if(profileKernels){
        std::printf("KERNELHOOK %s\n",KernelProfiler::Diagnostics().c_str());
        std::vector<std::pair<std::string,KernelTotal>> sorted(kernelTotals.begin(),kernelTotals.end());
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second.ms>b.second.ms;});
        double all=0;for(const auto& [n,t]:sorted)all+=t.ms;
        std::printf("KERNELS frames=%u kernels=%zu sum_ms_per_frame=%.4f\n",profiledFrames,sorted.size(),profiledFrames?all/profiledFrames:0.0);
        for(const auto& [n,t]:sorted)
            std::printf("KERNEL %8.4f ms/frame %5.1f%% calls/frame=%.1f max=%.4f grid=%ux%ux%u block=%ux%ux%u shared=%u %s\n",
                t.ms/profiledFrames,all>0?100.0*t.ms/all:0.0,double(t.count)/profiledFrames,t.maxMs,
                t.grid[0],t.grid[1],t.grid[2],t.block[0],t.block[1],t.block[2],t.shared,n.c_str());
    }
    for(size_t i=0;i<total.size();++i)std::printf("SAMPLE %zu %.6f %.6f\n",i,total[i],inference[i]);
    std::puts("CLEANUP begin: all frame work retired, releasing NeuralPass");
    std::fflush(stdout);
    } // All submissions retired before releasing the feature and normal loader.
    std::puts("CLEANUP feature released, shutting down normal NGX");std::fflush(stdout);
    if (bootstrapInitialized || TheosRenderPipeline::NeuralRendering::PublicNGXInitializedInProcess()) Require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_Shutdown1(gpu.device.Get())),"normal NGX shutdown");
    std::puts("CLEANUP NGX shutdown complete (public shutdown only when initialized)");std::fflush(stdout);
}
