#include "FrameGen/PresentationVSync.h"
#include "FrameGen/XeFGPacingAPI.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
using namespace TheosRenderPipeline::SourceDLSSG;
using Microsoft::WRL::ComPtr;
static void Require(bool value, const char* why) { if (!value) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
static void Check(HRESULT result, const char* why) { Require(SUCCEEDED(result),why); }
int main(int argc, char** argv) {
    unsigned cases{};
    for (auto mode : {VSyncMode::Auto,VSyncMode::Off,VSyncMode::On})
    for (bool meter : {false,true}) for (bool tearing : {false,true}) for (bool windowed : {false,true})
    for (UINT sync : {0u,1u,2u}) for (UINT flags : {0u,UINT(DXGI_PRESENT_ALLOW_TEARING),UINT(DXGI_PRESENT_DO_NOT_WAIT),UINT(DXGI_PRESENT_TEST|DXGI_PRESENT_ALLOW_TEARING)}) {
        const auto out=ResolveVSync({mode,meter,tearing},sync,flags,windowed);++cases;
        Require(out.incomingSync==sync && out.incomingFlags==flags,"incoming request retained for diagnostics");
        Require((out.flags & ~UINT(DXGI_PRESENT_ALLOW_TEARING))==(flags & ~UINT(DXGI_PRESENT_ALLOW_TEARING)),"other flags never discarded");
        if ((flags & DXGI_PRESENT_TEST) || (mode==VSyncMode::Auto && !meter)) {
            Require(out.sync==sync && out.flags==flags && !out.autoMetering,"probe/follow-caller path unchanged");
        } else if (mode==VSyncMode::On) {
            Require(out.sync==1 && !(out.flags & DXGI_PRESENT_ALLOW_TEARING) && !out.autoMetering,"VSync On removes conflicting tearing");
        } else {
            Require(out.sync==0 && bool(out.flags & DXGI_PRESENT_ALLOW_TEARING)==(tearing&&windowed),"immediate presentation uses only legal tearing");
            Require(out.autoMetering==(mode==VSyncMode::Auto),"automatic metering override reported separately");
        }
    }
    Require(SanitizeVSync(static_cast<VSyncMode>(99))==VSyncMode::Auto,"invalid mode defaults to Auto");
    // Live policy changes and provider return: Auto restores the caller request
    // when the active presenter has no metering owner.
    Require(ResolveVSync({VSyncMode::Auto,true,true},1,0,true).sync==0,"XeFG Auto overrides caller VSync");
    Require(ResolveVSync({VSyncMode::Auto,false,true},1,0,true).sync==1,"provider return restores caller VSync");
    // DLSS-G requirement: Auto turns VSync off while it generates, reported apart from metering.
    for (bool tearing : {false,true}) for (bool windowed : {false,true}) {
        const auto required=ResolveVSync({VSyncMode::Auto,false,tearing,true},1,0,windowed);
        Require(required.sync==0 && required.autoRequiredOff && !required.autoMetering,"required-off Auto submits immediate");
        Require(bool(required.flags & DXGI_PRESENT_ALLOW_TEARING)==(tearing&&windowed),"required-off uses only legal tearing");
        const auto metered=ResolveVSync({VSyncMode::Auto,true,tearing,true},1,0,windowed);
        Require(metered.sync==0 && metered.autoMetering && !metered.autoRequiredOff,"metering keeps its own report");
        Require(ResolveVSync({VSyncMode::On,false,tearing,true},1,0,windowed).sync==1,"explicit On stays the user's choice");
        const auto off=ResolveVSync({VSyncMode::Off,false,tearing,true},1,0,windowed);
        Require(off.sync==0 && !off.autoRequiredOff,"explicit Off is not reported as Auto");
        Require(ResolveVSync({VSyncMode::Auto,false,tearing,true},1,DXGI_PRESENT_TEST,windowed).sync==1,"probes stay untouched");
    }
    Require(ResolveVSync({VSyncMode::Auto,false,true,false},1,0,true).sync==1,"generation off restores caller VSync");
    using namespace TheosRenderPipeline;
    constexpr auto ready = XeFGPacing::Installed | XeFGPacing::Registered | XeFGPacing::DriverAvailable | XeFGPacing::QueueReady;
    Require(XeFGPacing::CanMeter(ready), "available pacing admits Auto before the first independent flip");
    for (auto unavailable : {UINT(XeFGPacing::Unavailable),UINT(XeFGPacing::Unsafe),UINT(XeFGPacing::Unavailable|XeFGPacing::Unsafe)}) {
        const auto meter=XeFGPacing::CanMeter(ready | unavailable);
        Require(!meter, "refused/disabled/unsafe metering is unavailable to Auto");
        const auto request=ResolveVSync({VSyncMode::Auto,meter,true},1,0,true);
        Require(request.sync==1 && request.flags==0 && !request.autoMetering,
            "driver refusal restores caller VSync and does not add tearing");
        Require(ResolveVSync({VSyncMode::Off,meter,true},1,0,true).sync==0,
            "explicit Off stays independent of metering refusal");
    }
    for (auto missing : {XeFGPacing::Installed,XeFGPacing::Registered,XeFGPacing::DriverAvailable,XeFGPacing::QueueReady})
        Require(!XeFGPacing::CanMeter(ready & ~UINT(missing)), "Auto needs every pacing prerequisite");
    std::printf("PASS %u presentation policy cases, probe preservation, live/provider changes\n",cases);
    if (argc==1) return 0;
    Require(argc==2 && std::strcmp(argv[1],"--gpu")==0,"explicit GPU option");
    auto window=CreateWindowExW(0,L"STATIC",L"TRP presentation policy fixture",WS_OVERLAPPEDWINDOW,0,0,400,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Require(window!=nullptr,"fixture window");ShowWindow(window,SW_SHOWNOACTIVATE);
    ComPtr<IDXGIFactory5> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    BOOL tearing{};Check(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing)),"tearing capability");
    ComPtr<ID3D12Device> device;Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"hardware device");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=400;desc.Height=240;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags=tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> chain;Check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&chain),"native flip chain");
    for (bool present1 : {false,true}) for (auto mode : {VSyncMode::Auto,VSyncMode::On,VSyncMode::Off,VSyncMode::Auto}) {
        BOOL fullscreen=TRUE;Check(chain->GetFullscreenState(&fullscreen,nullptr),"fullscreen query");
        auto out=ResolveVSync({mode,true,tearing!=FALSE},1,0,!fullscreen);
        DXGI_PRESENT_PARAMETERS parameters{};
        const auto hr=present1 ? chain->Present1(out.sync,out.flags,&parameters) : chain->Present(out.sync,out.flags);
        std::printf("native Present%s mode=%s incoming=1 submitted=%u flags=0x%X result=0x%08lX\n",present1?"1":"",VSyncName(mode),out.sync,out.flags,hr);
        Check(hr,"native Present accepts policy flags");
    }
    ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    Check(queue->Signal(fence.Get(),1),"retire fixture queue");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"event");
    Check(fence->SetEventOnCompletion(1,event),"retirement event");Require(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"retirement completed");CloseHandle(event);
    chain.Reset();DestroyWindow(window);std::puts("PASS native Present/Present1 live Auto/On/Off transitions");
}
