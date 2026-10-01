#pragma once
// Independent public-API experiment; does not hook or deploy into Skyrim.
#include "FrameGen/SourceDLSSGInterop.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <DirectXPackedVector.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xess_fg/xefg_swapchain_debug.h>
#include <xell/xell_d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline::SourceDLSSG;

static void Require(bool ok, const char* stage) {
    if (!ok) {
        std::fprintf(stderr, "FAIL stage=%s\n", stage);
        // A failed GPU retirement must not unwind and free GPU-owned objects.
        std::fflush(nullptr); ExitProcess(1);
    }
}
static void Check(HRESULT hr, const char* stage) {
    std::printf("HRESULT stage=%s result=0x%08lX\n", stage, static_cast<unsigned long>(hr));
    Require(SUCCEEDED(hr), stage);
}
static void FG(xefg_swapchain_result_t r, const char* stage) {
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        std::printf("XeFG stage=%s result=%d\n", stage, static_cast<int>(r));
    Require(r >= 0, stage);
}
static void LL(xell_result_t r, const char* stage) {
    if (r != XELL_RESULT_SUCCESS)
        std::printf("XeLL stage=%s result=%d\n", stage, static_cast<int>(r));
    Require(r == XELL_RESULT_SUCCESS, stage);
}
template<class T> static T Export(HMODULE dll, const char* name) {
    auto p=GetProcAddress(dll,name); Require(p!=nullptr,name); return reinterpret_cast<T>(p);
}
static HMODULE Load(const std::filesystem::path& path) {
    auto dll=LoadLibraryExW(path.c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    std::printf("runtime path=%ls loadError=%lu\n",path.c_str(),dll?0:GetLastError());
    Require(dll!=nullptr,"load exact runtime and dependencies"); return dll;
}
#define FG_EXPORT(name) decltype(&name) name=Export<decltype(&::name)>(fgDll,#name)
#define LL_EXPORT(name) decltype(&name) name=Export<decltype(&::name)>(llDll,#name)
struct Api {
    HMODULE llDll{}, fgDll{};
    explicit Api(const std::filesystem::path& path) :
        llDll(Load(path/L"libxell.dll")),fgDll(Load(path/L"libxess_fg.dll")) {}
    LL_EXPORT(xellGetVersion);
    LL_EXPORT(xellD3D12CreateContext);
    LL_EXPORT(xellSetSleepMode);
    LL_EXPORT(xellSleep);
    LL_EXPORT(xellAddMarkerData);
    LL_EXPORT(xellDestroyContext);
    FG_EXPORT(xefgSwapChainGetVersion);
    FG_EXPORT(xefgSwapChainD3D12CreateContext);
    FG_EXPORT(xefgSwapChainSetLoggingCallback);
    FG_EXPORT(xefgSwapChainSetLatencyReduction);
    FG_EXPORT(xefgSwapChainGetProperties);
    FG_EXPORT(xefgSwapChainD3D12GetProperties);
    FG_EXPORT(xefgSwapChainD3D12InitFromSwapChainDesc);
    FG_EXPORT(xefgSwapChainD3D12GetSwapChainPtr);
    FG_EXPORT(xefgSwapChainD3D12TagFrameResource);
    FG_EXPORT(xefgSwapChainTagFrameConstants);
    FG_EXPORT(xefgSwapChainSetPresentId);
    FG_EXPORT(xefgSwapChainSetEnabled);
    FG_EXPORT(xefgSwapChainSetNumInterpolatedFrames);
    FG_EXPORT(xefgSwapChainGetLastPresentStatus);
    FG_EXPORT(xefgSwapChainDestroy);
    FG_EXPORT(xefgSwapChainEnableDebugFeature);
    FG_EXPORT(xefgSwapChainSetUiCompositionState);
    ~Api() { FreeLibrary(fgDll); FreeLibrary(llDll); }
};
#undef FG_EXPORT
#undef LL_EXPORT

static void Log(const char* message, xefg_swapchain_logging_level_t level, void*) {
    std::printf("SDK level=%d %s\n", static_cast<int>(level),message);
}
static LRESULT CALLBACK WindowProc(HWND w, UINT m, WPARAM a, LPARAM b) {
    return DefWindowProcW(w,m,a,b);
}
struct Inputs {
    SharedTexture color, hudless, depth, motion;
    ComPtr<ID3D11Texture2D> producerColor, producerHudless, producerDepth, producerMotion;
    UINT width{}, height{};
    void Create(Interop& interop, ID3D11Device* device, UINT w, UINT h) {
        width=w; height=h;
        auto create=[&](DXGI_FORMAT format, SharedTexture& shared, ComPtr<ID3D11Texture2D>& producer) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width=w; d.Height=h; d.MipLevels=1; d.ArraySize=1;
            d.Format=format; d.SampleDesc.Count=1; d.Usage=D3D11_USAGE_DEFAULT;
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
            Check(interop.CreateSharedTexture(d,shared),"production shared texture");
            Check(device->CreateTexture2D(&d,nullptr,&producer),"synthetic D3D11 producer");
        };
        create(DXGI_FORMAT_R8G8B8A8_UNORM,color,producerColor);
        create(DXGI_FORMAT_R8G8B8A8_UNORM,hudless,producerHudless);
        create(DXGI_FORMAT_R32_FLOAT,depth,producerDepth);
        create(DXGI_FORMAT_R16G16_FLOAT,motion,producerMotion);
    }
    void Paint(ID3D11DeviceContext* context, unsigned frame) {
        const auto pixels=static_cast<size_t>(width)*height;
        std::vector<uint32_t> colors(pixels);
        std::vector<uint32_t> hudlessColors(pixels);
        std::vector<float> depths(pixels,0.5f);
        std::vector<uint16_t> motions(pixels*2,0);
        // An orthographic plane translating two screen pixels per source frame.
        // Motion is current->previous in pixel units, excluding the fixed HUD.
        const auto velocity=DirectX::PackedVector::XMConvertFloatToHalf(-2.0f);
        for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) {
            const auto n=static_cast<size_t>(y)*width+x;
            const bool bright=(((x+width-frame*2%width)/32+y/32)%2)==0;
            colors[n]=bright?0xff7098d0:0xff382820;
            hudlessColors[n]=colors[n];
            motions[n*2]=velocity;
            // Depth/MV describe the HUD-less plane even underneath the UI.
            if (x<96 && y<24) colors[n]=0xff20c060;
        }
        context->UpdateSubresource(producerColor.Get(),0,nullptr,colors.data(),width*4,0);
        context->UpdateSubresource(producerHudless.Get(),0,nullptr,hudlessColors.data(),width*4,0);
        context->UpdateSubresource(producerDepth.Get(),0,nullptr,depths.data(),width*4,0);
        context->UpdateSubresource(producerMotion.Get(),0,nullptr,motions.data(),width*4,0);
    }
};

