// Render hooks derived from PureDark's MIT-licensed Skyrim-Upscaler
// (https://github.com/PureDark/Skyrim-Upscaler).

#include <PCH.h>
#include "UpscalerDeviceHooks.h"
#include "UpscalerHooks.h"
#include "DLSSBackend.h"
#include "RenderPipeline.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/SourceHostBoundary.h"
#include "OverlayUI.h"
#include "PerformanceTuning.h"
#include "CommunityShaderIntegration.h"

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChain;
decltype(&IDXGIFactory::CreateSwapChain) ptrFactoryCreateSwapChain;

void BeforeGameSwapChainPresent(IDXGISwapChain* a_swapChain)
{
    // Alt-tab freeze diagnosis: a long gap between presents tells us whether
    // the game loop wedged (gap here) or only the display path stalled.
    static LARGE_INTEGER lastPresent{};
    static LARGE_INTEGER qpcFreq{};
    if (!qpcFreq.QuadPart)
    {
        ::QueryPerformanceFrequency(&qpcFreq);
    }
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    if (lastPresent.QuadPart)
    {
        const double gap =
            static_cast<double>(now.QuadPart - lastPresent.QuadPart) / static_cast<double>(qpcFreq.QuadPart);
        if (gap > 1.0)
        {
            logger::info("[Present] gap of {:.2f}s between presents", gap);
        }
    }
    lastPresent = now;

    auto* nvidiaHost = NvidiaHost::GetSingleton();
    if (TheosRenderPipeline::CommunityShaders::Active()) {
        nvidiaHost->PrepareCommunityFrameForPresent();
        PerformanceTuning::GetSingleton()->EndD3D11Frame(RenderPipeline::GetSingleton()->mContext);
        return;
    }
    if (nvidiaHost->ProxyActive() && nvidiaHost->StartupConfigured())
    {
        nvidiaHost->PrepareSourceFrameForPresent(a_swapChain);
        OverlayUI::GetSingleton()->OnPresent();
        nvidiaHost->FinishNativeUIPassForPresent();
    }
    else
    {
        OverlayUI::GetSingleton()->OnPresent();
    }
    // End GPU work before the potentially blocking native Present. Leaving this
    // open until the next renderer begin includes presentation/idle time.
    PerformanceTuning::GetSingleton()->EndD3D11Frame(RenderPipeline::GetSingleton()->mContext);
}

HRESULT WINAPI hk_IDXGIFactory_CreateSwapChain(IDXGIFactory* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                               IDXGISwapChain** ppSwapChain)
{
    auto nvidiaHost = NvidiaHost::GetSingleton();

    // The vtable detour is class-wide: later swapchain creations from other
    // components must not re-enter proxy setup over live state.
    if (nvidiaHost->ProxyActive())
    {
        logger::info("[FrameGen] additional CreateSwapChain after proxy setup; passing through");
        return (This->*ptrFactoryCreateSwapChain)(pDevice, pDesc, ppSwapChain);
    }

    ID3D11Device* d3d11Device = nullptr;
    if (!pDesc || !ppSwapChain || !pDevice || FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&d3d11Device))))
    {
        logger::warn("[FrameGen] factory CreateSwapChain without a D3D11 device; passing through");
        return (This->*ptrFactoryCreateSwapChain)(pDevice, pDesc, ppSwapChain);
    }

    // The stable render-sized buffer is allocated inside CreateSwapChain, so
    // load its quality/sharpening contract before that one-way size decision.
    RenderPipeline::GetSingleton()->LoadINI();
    const auto result = nvidiaHost->CreateSwapChain(This, d3d11Device, pDesc, ppSwapChain);
    if (SUCCEEDED(result))
    {
        d3d11Device->Release();
        logger::info("[NvidiaHost] game-facing swapchain ownership acquired");
        return result;
    }

    // Streamline may already own a native swapchain on this HWND. Do not
    // construct a second owner after a partially successful startup.
    logger::critical("[SourceDLSSG] startup failed; refusing mixed-owner fallback: {}", nvidiaHost->Status());
    d3d11Device->Release();
    return result;
}

HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                                UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                                UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                D3D_FEATURE_LEVEL* pFeatureLevel,
                                                ID3D11DeviceContext** ppImmediateContext)
{
    logger::info("Calling original D3D11CreateDeviceAndSwapChain");
    TheosRenderPipeline::CommunityShaders::InstallEngineHooks();

    // DLSS and native UI require the host's stable game-facing buffer, including
    // sessions that start with interpolation off. Install before device creation.
    if (!pSwapChainDesc || !ppSwapChain || !ppDevice || !ppImmediateContext)
    {
        util::report_and_fail("Theo's Render Pipeline requires a game device and swapchain creation request.");
    }
    if (!pSwapChainDesc->Windowed)
    {
        util::report_and_fail("Theo's Render Pipeline requires windowed or borderless mode. Disable exclusive fullscreen and restart Skyrim.");
    }
    auto frameGen = SourceFrameGeneration::GetSingleton();
    frameGen->refreshRate = SourceFrameGeneration::GetRefreshRate(pSwapChainDesc->OutputWindow);
    logger::info("[NvidiaHost] display refresh rate: {:.1f} Hz; host required with interpolation {}",
                 frameGen->refreshRate, frameGen->RuntimeInterpolationRequested() ? "on" : "off");
    if (!ptrFactoryCreateSwapChain)
    {
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;
        if (pAdapter)
        {
            pAdapter->GetParent(IID_PPV_ARGS(&factory));
        }
        if (!factory)
        {
            CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        }
        if (factory)
        {
            // Class-level vtable patch: affects the factory instance DXGI uses
            // internally for this same factory type.
            *(uintptr_t*)&ptrFactoryCreateSwapChain =
                Detours::X64::DetourClassVTable(*(uintptr_t*)factory.Get(), &hk_IDXGIFactory_CreateSwapChain, 10);
            if (!ptrFactoryCreateSwapChain)
            {
                util::report_and_fail("Theo's Render Pipeline could not install its required NVIDIA swapchain hook.");
            }
            logger::info("[FrameGen] IDXGIFactory::CreateSwapChain detoured for proxying");
        }
        else
        {
            util::report_and_fail("Theo's Render Pipeline could not obtain the DXGI factory required for NVIDIA presentation.");
        }
    }

    if (ppSwapChain)
    {
        *ppSwapChain = nullptr;
    }
    if (ppDevice)
    {
        *ppDevice = nullptr;
    }
    if (ppImmediateContext)
    {
        *ppImmediateContext = nullptr;
    }
    HRESULT hr = (*ptrD3D11CreateDeviceAndSwapChain)(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                                                     FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice,
                                                     pFeatureLevel, ppImmediateContext);

    auto* nvidiaHost = NvidiaHost::GetSingleton();
    hr = TheosRenderPipeline::CompleteDeviceCreation(
        hr, ppSwapChain, ppDevice, ppImmediateContext,
        [&]
        {
            return TheosRenderPipeline::CompleteRequiredHostStartup(*nvidiaHost);
        });
    if (FAILED(hr))
    {
        // The completion boundary has cleared failed outputs. Show the actual
        // startup error once, instead of leaving the game to fail without context.
        util::report_and_fail(std::format("Theo's Render Pipeline could not start rendering.\n\n{}\nHRESULT: 0x{:08X}\n\n"
                                          "See TheosRenderPipeline.log for details. Skyrim will close after this message.",
                                          nvidiaHost->Status(), (uint32_t)hr));
    }

    auto device = *ppDevice;
    auto deviceContext = *ppImmediateContext;
    auto swapChain = *ppSwapChain;
    // ENB may return a wrapper distinct from the host's inner immediate
    // context. These are the very interfaces whose vtables we hook below.
    nvidiaHost->RegisterSourceGameContext(deviceContext);
    RenderPipeline::GetSingleton()->SetupSwapChain(swapChain);
    RenderPipeline::GetSingleton()->PreInit();
    OverlayUI::GetSingleton()->Init(swapChain, device, deviceContext);
    logger::info("Detouring virtual function tables");
    // D3D11CreateDeviceAndSwapChain can return its own COM wrapper around our
    // stable outer swapchain, so pointer identity is not reliable here. The
    // active source proxy always reaches our outer Present and owns this work.
    logger::info("[NvidiaHost] outer game-facing swapchain owns the Present lifecycle");
    if (TheosRenderPipeline::CommunityShaders::Active()) {
        TheosRenderPipeline::CommunityShaders::InstallDeviceHooks(deviceContext, swapChain);
    } else {
        InstallUpscalerContextHooks(device, deviceContext);
    }

    return hr;
}

namespace TheosRenderPipeline
{
void InstallUpscalerDeviceHooks(std::uintptr_t moduleBase)
{
    // Continue through the previous import target so an earlier renderer's
    // device setup still runs before control returns to our completion boundary.
    ptrD3D11CreateDeviceAndSwapChain = reinterpret_cast<decltype(ptrD3D11CreateDeviceAndSwapChain)>(
        Detours::IATHook(moduleBase, "d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                         reinterpret_cast<uintptr_t>(hk_D3D11CreateDeviceAndSwapChain)));
    if (!ptrD3D11CreateDeviceAndSwapChain)
    {
        util::report_and_fail("Theo's Render Pipeline could not hook D3D11 device creation for its required NVIDIA host.");
    }
}
} // namespace TheosRenderPipeline
