#include "FrameGen/XeFGTensorRuntime.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <string_view>
using Microsoft::WRL::ComPtr;
static void Require(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL %s\n",message);ExitProcess(1);}}
int wmain(int argc,wchar_t** argv){
    Require(argc==3,"runtime directory and injected failure mode");
    SetEnvironmentVariableW(L"TRP_XEFG_MOCK_MODE",argv[2]);
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;
    Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumAdapters1(0,&adapter)),"hardware adapter");
    Require(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))),"test device");
    TheosRenderPipeline::XeFGTensorRuntime runtime;
    Require(runtime.Select(device.Get(),std::filesystem::absolute(argv[1]),true),"real loader selects mock extension after bundle admission");
    const auto first=runtime.Start(device.Get(),nullptr,nullptr);
    const auto mode=std::wstring_view(argv[2]);
    if(mode.starts_with(L"pacing-")){
        Require(SUCCEEDED(first),"mock model initialization");
        auto* context=reinterpret_cast<void*>(0x12340); // Mock receives identity only, never dereferences it.
        const auto registered=runtime.RegisterPacing(context,device.Get(),nullptr);
        if(mode==L"pacing-api-version" || mode==L"pacing-safe-register") {
            Require(registered==S_FALSE&&!runtime.Unsafe(),"clean/absent pacing is compatible with existing tensor helper");
        }else if(mode==L"pacing-v1") {
            Require(registered==S_OK&&!runtime.Unsafe(),"a version-1 helper keeps pacing registration and metering");
            Require(!runtime.VSyncCadenceSupported(),"a version-1 helper has no VSync cadence");
            runtime.SetVSyncCadence(true);
            Require(!(runtime.PacingStats().flags & TheosRenderPipeline::XeFGPacing::VSyncCadenceRequested),"VSync cadence stays off with a version-1 helper");
            runtime.SetVSyncCadence(false);
            Require(SUCCEEDED(runtime.UnregisterPacing(context))&&SUCCEEDED(runtime.Retire()),"version-1 pacing and tensor retire cleanly");
        }else if(mode==L"pacing-unsafe-register") {
            Require(FAILED(registered)&&runtime.Unsafe(),"failed publication with retained pacing owner is unsafe");
            Require(FAILED(runtime.Retire())&&runtime.Snapshot().cachedPSOs==0,"retained pacing blocks tensor cleanup");
        }else {
            Require(SUCCEEDED(registered),"pacing registered");
            runtime.SetVSyncCadence(true);
            Require(runtime.PacingStats().flags & TheosRenderPipeline::XeFGPacing::VSyncCadenceRequested,"runtime forwards live on");
            runtime.SetVSyncCadence(false);
            Require(!(runtime.PacingStats().flags & TheosRenderPipeline::XeFGPacing::VSyncCadenceRequested),"runtime forwards live off");
            runtime.SetFlipMetering(false);runtime.ResetPacing(context);
            const auto retired=runtime.UnregisterPacing(context);
            if(mode==L"pacing-retire-fail") {
                Require(FAILED(retired)&&runtime.Unsafe(),"pacing retirement failure latches unsafe without helper Unsafe flag");
                Require(FAILED(runtime.Start(device.Get(),nullptr,nullptr))&&FAILED(runtime.Retire()),"no replacement or cleanup after failed pacing retirement");
                Require(runtime.Snapshot().matchedPSOs==1&&runtime.Snapshot().cachedPSOs==0,"no later extension callbacks");
            }else {
                Require(SUCCEEDED(retired)&&!runtime.Unsafe(),"pacing retires cleanly");
                runtime.SetVSyncCadence(true);
                Require(runtime.RegisterPacing(context,device.Get(),nullptr)==S_OK &&
                    (runtime.PacingStats().flags & TheosRenderPipeline::XeFGPacing::VSyncCadenceRequested),
                    "replacement owner registers in startup-enabled state");
                Require(SUCCEEDED(runtime.UnregisterPacing(context)),"enabled replacement retires");
                runtime.SetVSyncCadence(false);
                Require(runtime.RegisterPacing(context,device.Get(),nullptr)==S_OK &&
                    !(runtime.PacingStats().flags & TheosRenderPipeline::XeFGPacing::VSyncCadenceRequested),
                    "replacement owner registers in default startup state");
                Require(SUCCEEDED(runtime.UnregisterPacing(context)),"off replacement retires");
                Require(SUCCEEDED(runtime.Retire())&&runtime.Snapshot().cachedPSOs==1,"tensor cleanup follows pacing retirement");
            }
        }
        std::printf("PASS pacing lifetime mode=%ls unsafe=%u\n",argv[2],runtime.Unsafe());return 0;
    }
    const bool safe=std::wstring_view(argv[2])==L"safe-start";
    const bool unsafeStart=std::wstring_view(argv[2])==L"unsafe-start";
    if(safe){
        Require(FAILED(first)&&!runtime.Unsafe()&&!runtime.ModelReady(),"clean initialization rejection can fall back");
        Require(SUCCEEDED(runtime.Retire())&&runtime.Snapshot().cachedPSOs==0,"inactive clean rejection needs no retirement callback");
    }else{
        Require(unsafeStart?FAILED(first):SUCCEEDED(first),"injected initialization outcome");
        Require(FAILED(runtime.Retire())&&runtime.Unsafe(),"retirement failure is unsafe even if extension omits its flag");
        const auto before=runtime.Snapshot();
        Require(FAILED(runtime.Start(device.Get(),nullptr,nullptr)),"uncertain owner prevents replacement");
        Require(FAILED(runtime.Retire()),"uncertain retirement is not retried");
        const auto after=runtime.Snapshot();
        Require(before.matchedPSOs==after.matchedPSOs&&before.cachedPSOs==after.cachedPSOs,"failed owner retains all state without new extension calls");
    }
    Require(GetModuleHandleW(L"trp_xefg_tensor.dll")!=nullptr,"extension remains process resident");
    std::printf("PASS tensor lifetime mode=%ls unsafe=%u starts=%llu retires=%llu (mock callbacks; no model execution)\n",argv[2],runtime.Unsafe(),runtime.Snapshot().matchedPSOs,runtime.Snapshot().cachedPSOs);
}
