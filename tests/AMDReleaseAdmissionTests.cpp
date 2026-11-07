#include "FrameGen/AmdNeuralPass.h"
#include "FrameGen/NeuralRuntimeSession.h"
#include "FrameGen/SourceDLSSGSettings.h"
#include <dxgi1_6.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
using namespace TheosRenderPipeline;
using Microsoft::WRL::ComPtr;
static void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int main(int argc, char**) {
    try {
        if (argc > 1) {
            ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter; ComPtr<ID3D12Device> device;
            Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
            bool found{};
            for (UINT i=0; factory->EnumAdapters1(i,&adapter)==S_OK; ++i) {
                DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
                if (desc.VendorId==0x10DE && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) { found=true; break; }
                adapter.Reset();
            }
            Require(found && SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))), "NVIDIA hardware device");
            Require(!SourceDLSSG::AmdNeuralPass::IsAMD(device.Get()), "NVIDIA cannot enter HIP");
            Require(SourceDLSSG::AmdNeuralPass::Availability(false)!=nullptr, "HIP refuses NVIDIA");
            for (const auto* name : {L"amdhip64.dll",L"amdhip64_6.dll",L"amdhip64_7.dll",L"dlss5nr_runtime.dll",L"zluda_real.dll"})
                Require(!GetModuleHandleW(name), "Refusal must not load HIP/ZLUDA");
            std::cout << "PASS: HIP admission refuses NVIDIA without loading HIP/ZLUDA\n";
            return 0;
        }
        using namespace NeuralRendering;
        for (const auto first : {Backend::HIP,Backend::Vulkan}) {
            RuntimeSession state;
            const auto other=first==Backend::HIP?Backend::Vulkan:Backend::HIP;
            Require(!state.Claim(first),"first runtime admitted");
            Require(!state.Claim(first),"same runtime can recreate");
            Require(state.Claim(other)==kRuntimeRestart,"competing runtime requires restart");
            Require(state.Refusal(other)==kRuntimeRestart,"menu matches runtime guard");
        }
        RuntimeSession probing;
        Require(probing.BeginVulkanProbe(),"probe begins before either backend");
        Require(probing.Claim(Backend::HIP)==kProbeFinishing,"HIP cannot overlap capability query");
        probing.EndVulkanProbe();
        Require(!probing.Claim(Backend::HIP),"HIP enters after probe retirement");
        Require(!probing.BeginVulkanProbe(),"no new Vulkan query after HIP");
        for (int i=0;i<100;++i) {
            RuntimeSession state; std::atomic_int admitted{};
            std::thread a([&]{if(!state.Claim(Backend::HIP))++admitted;});
            std::thread b([&]{if(!state.Claim(Backend::Vulkan))++admitted;});
            a.join();b.join();Require(admitted==1,"concurrent claims have exactly one owner");
        }
        SourceDLSSG::Preferences amd,nvidia,custom;
        SourceDLSSG::ApplyNeuralHardwareDefaults(amd,true);
        SourceDLSSG::ApplyNeuralHardwareDefaults(nvidia,false);
        Require(amd.neuralBackend==Backend::HIP && amd.neuralReconstruction.inputScale==.5f && amd.amdNRWaitForNetwork,"AMD fresh-install defaults");
        Require(nvidia.neuralBackend==Backend::Native && nvidia.neuralReconstruction.inputScale==1,"NVIDIA defaults retained");
        custom.neuralBackendAutomatic=custom.neuralInputAutomatic=false;
        custom.neuralBackend=Backend::Vulkan;custom.neuralReconstruction.inputScale=.75f;custom.amdNRWaitForNetwork=false;
        SourceDLSSG::ApplyNeuralHardwareDefaults(custom,true);
        Require(custom.neuralBackend==Backend::Vulkan && custom.neuralReconstruction.inputScale==.75f && !custom.amdNRWaitForNetwork,"explicit choices retained");
        std::cout << "PASS: hardware defaults, explicit preferences, exclusive claims and capability-query retirement\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
