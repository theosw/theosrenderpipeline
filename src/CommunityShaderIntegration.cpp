#include <PCH.h>
#include "CommunityShaderIntegration.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/CommunityShaderAdapter.h"
#include "RenderPipeline.h"
#include "OverlayUI.h"
#include "PerformanceTuning.h"
#include "HookInstallation.h"
#include "SkyrimRuntime.h"

namespace TheosRenderPipeline::CommunityShaders
{
    namespace
    {
        bool active{}, installed{};
        std::uintptr_t engineTarget{};
        thread_local bool worldBoundary{};
        using PostProcessing = void (*)(RE::ImageSpaceManager*, std::uint32_t, RE::RENDER_TARGET, void*, bool);
        PostProcessing engineOriginal{}, producerOriginal{};
        CommunityShaderFrame::Dispatch dispatchOriginal{};
        using Copy = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
        Copy copyOriginal{};
        using Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        Present presentOriginal{};
        IDXGISwapChain* gameSwapChain{};
        std::uintptr_t Callsite()
        {
            const auto* profile = SkyrimRuntime::Find(REL::Module::get().version());
            if (!profile) { util::report_and_fail("No verified CS postprocessing profile for this Skyrim runtime."); }
            return REL::RelocationID(100430, 107148).address() + profile->hooks.csPostProcessing;
        }
        ID3D11Texture2D* World()
        {
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            return renderer ? reinterpret_cast<ID3D11Texture2D*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture) : nullptr;
        }
        Microsoft::WRL::ComPtr<ID3D11Texture2D> Framebuffer()
        {
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            // CommonLib exposes the standard COM interfaces through REX types.
            return renderer ? CommunityShaderFrame::Texture(reinterpret_cast<ID3D11RenderTargetView*>(
                renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER].RTV)) : nullptr;
        }
        void EnginePostProcessing(RE::ImageSpaceManager* manager, std::uint32_t effect, RE::RENDER_TARGET target, void* arg, bool flag)
        {
            const bool capture = worldBoundary;
            // Do not mistake nested imagespace calls for another frame boundary.
            worldBoundary = false;
            auto& adapter = NvidiaHost::GetSingleton()->CommunityFrame();
            const bool ready = capture && adapter.AfterUpscaling();
            engineOriginal(manager, effect, target, arg, flag);
            if (ready) { adapter.CompleteWorld(Framebuffer().Get()); }
            worldBoundary = capture;
        }
        void ProducerPostProcessing(RE::ImageSpaceManager* manager, std::uint32_t effect, RE::RENDER_TARGET target, void* arg, bool flag)
        {
            auto* host = NvidiaHost::GetSingleton();
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            auto* state = reinterpret_cast<BSGraphics::State*>(RE::BSGraphics::State::GetSingleton());
            auto* pipeline = RenderPipeline::GetSingleton();
            CommunityShaderAdapter::Input input{};
            if (renderer && state && host->UpscalerReady()) {
                auto& data = state->GetRuntimeData();
                const auto width = data.dynamicResolutionWidthRatio * host->OutputWidth();
                const auto height = data.dynamicResolutionHeightRatio * host->OutputHeight();
                if (std::isfinite(width) && std::isfinite(height) && width >= 1 && height >= 1 &&
                    width <= host->OutputWidth() && height <= host->OutputHeight()) {
                    input.render = {static_cast<UINT>(std::lround(width)), static_cast<UINT>(std::lround(height))};
                }
                input.output = {host->OutputWidth(), host->OutputHeight()};
                input.context = pipeline->mContext; input.graphics = state; input.world = World();
                input.motion = reinterpret_cast<ID3D11Texture2D*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].texture);
                input.depth = reinterpret_cast<ID3D11Texture2D*>(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture);
                input.frame = state->GetFrameCount(); input.jittered = GetGameTAA();
                input.jitterX = state->jitter[0] * input.render.width / 2.0f;
                input.jitterY = -state->jitter[1] * input.render.height / 2.0f;
                input.reset = pipeline->mPendingHistoryResets > 0;
                auto* ui = RE::UI::GetSingleton();
                input.worldEligible = ui && !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
                pipeline->mGraphicsState = state;
                pipeline->mRenderedFrameCount = input.frame;
                pipeline->mRenderSizeX = input.render.width; pipeline->mRenderSizeY = input.render.height;
                PerformanceTuning::GetSingleton()->BeginD3D11Frame(pipeline->mDevice, pipeline->mContext, input.frame);
            }
            const bool previous = worldBoundary;
            worldBoundary = input.context && host->CommunityFrame().BeginWorld(input);
            producerOriginal(manager, effect, target, arg, flag);
            worldBoundary = previous;
        }
        void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
        {
            if (!ReShadeIntegration::Get().Internal()) {
                NvidiaHost::GetSingleton()->CommunityFrame().CaptureDisplayTransform(context, x, y, z, dispatchOriginal);
            }
            dispatchOriginal(context, x, y, z);
        }
        void STDMETHODCALLTYPE CopyResource(ID3D11DeviceContext* context, ID3D11Resource* destination, ID3D11Resource* source)
        {
            auto* host = NvidiaHost::GetSingleton();
            if (!ReShadeIntegration::Get().Internal() && D3D11FrameCopy::SameObject(destination, host->GameFacingTexture())) {
                host->CommunityFrame().ConfirmPresentationCopy(source);
            }
            copyOriginal(context, destination, source);
        }
        HRESULT STDMETHODCALLTYPE TopPresent(IDXGISwapChain* chain, UINT interval, UINT flags)
        {
            if (chain == gameSwapChain && !(flags & DXGI_PRESENT_TEST)) {
                auto frame = Framebuffer();
                NvidiaHost::GetSingleton()->CommunityFrame().SetUIBoundary(frame.Get());
                OverlayUI::GetSingleton()->OnPresent(frame.Get());
            }
            return presentOriginal(chain, interval, flags);
        }
    }

    bool Active() { return active; }
    void RememberEngineBoundary()
    {
        const auto site = Callsite();
        const auto target = HookSafety::DirectCallTarget(site);
        if (!target) { return; }
        HMODULE owner{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(target), &owner) && owner == GetModuleHandleW(nullptr)) { engineTarget = target; }
    }
    void SelectRenderer()
    {
        active = GetModuleHandleW(L"CommunityShaders.dll") != nullptr;
        logger::info("[Renderer] world/upscaling owner={}", active ? "Community Shaders" : "Theo's Render Pipeline");
    }
    void InstallEngineHooks()
    {
        if (!active || installed) { return; }
        if (!engineTarget || !HookSafety::DirectCallTarget(Callsite())) {
            util::report_and_fail("Community Shaders postprocessing boundary is unavailable; no frame adapter hooks were installed.");
        }
        engineOriginal = reinterpret_cast<PostProcessing>(HookSafety::InstallEntryDetour(engineTarget,
            reinterpret_cast<std::uintptr_t>(&EnginePostProcessing)));
        if (!engineOriginal) { util::report_and_fail("Could not preserve the engine postprocessing function for Community Shaders."); }
        producerOriginal = reinterpret_cast<PostProcessing>(SKSE::GetTrampoline().write_call<5>(Callsite(), &ProducerPostProcessing));
        installed = true;
        logger::info("[CS Adapter] engine postprocessing chain installed; CS owns upscaling, jitter and render scale");
    }
    void InstallDeviceHooks(ID3D11DeviceContext* context, IDXGISwapChain* chain)
    {
        InstallVTableHook(context, 41, &Dispatch, dispatchOriginal);
        InstallVTableHook(context, 47, &CopyResource, copyOriginal);
        InstallVTableHook(chain, 8, &TopPresent, presentOriginal);
        gameSwapChain = chain;
        if (!dispatchOriginal || !copyOriginal || !presentOriginal) {
            util::report_and_fail("Could not preserve the CS display/Present chain.");
        }
    }
}
