#include <PCH.h>
#include "CommunityShaderIntegration.h"
#include "CommunityShaderUIBoundary.h"
#include "HookDetour.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/CommunityShaderAdapter.h"
#include "FrameGen/D3D11LiveSlot.h"
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
        using DrawInterface = void (*)(std::int64_t);
        DrawInterface interfaceOriginal{};
        CommunityShaderUIBoundary uiBoundary;
        CommunityShaderFrame::Dispatch dispatchOriginal{};
        using Copy = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
        Copy copyOriginal{};
        // The context's table is rewritten on every state swap, including our
        // isolation's. These entries are reinstalled on the renderer's context.
        D3D11LiveSlot liveDispatch, liveCopy;
        ID3D11DeviceContext* deviceContext{};
        ID3D11DeviceContext* producerContext{};
        // One observation per producer call, even when one hooked table forwards to another.
        thread_local unsigned contextHookDepth{};
        std::atomic<std::uint64_t> observedDispatches{}, observedCopies{}, observedPresents{};
        ID3D11DeviceContext* ProducerContext(RE::BSGraphics::Renderer* renderer);
        void ReportDisplayWait();
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
                input.producerContext = ProducerContext(renderer);
                ReportDisplayWait();
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
        struct ContextHookScope
        {
            ContextHookScope() { ++contextHookDepth; }
            ~ContextHookScope() { --contextHookDepth; }
        };
        void ObserveDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z, CommunityShaderFrame::Dispatch original)
        {
            if (contextHookDepth == 0) {
                observedDispatches.fetch_add(1, std::memory_order_relaxed);
                ContextHookScope scope;
                if (!ReShadeIntegration::Get().Internal()) {
                    NvidiaHost::GetSingleton()->CommunityFrame().CaptureDisplayTransform(context, x, y, z, original);
                }
            }
            ContextHookScope scope;
            original(context, x, y, z);
        }
        void ObserveCopy(ID3D11DeviceContext* context, ID3D11Resource* destination, ID3D11Resource* source, Copy original)
        {
            if (contextHookDepth == 0) {
                observedCopies.fetch_add(1, std::memory_order_relaxed);
                auto* host = NvidiaHost::GetSingleton();
                if (!ReShadeIntegration::Get().Internal() && D3D11FrameCopy::SameObject(destination, host->GameFacingTexture())) {
                    host->CommunityFrame().ConfirmPresentationCopy(source);
                }
            }
            ContextHookScope scope;
            original(context, destination, source);
        }
        void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
        {
            ObserveDispatch(context, x, y, z, dispatchOriginal);
        }
        void STDMETHODCALLTYPE LiveDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
        {
            ObserveDispatch(context, x, y, z, reinterpret_cast<CommunityShaderFrame::Dispatch>(liveDispatch.Original()));
        }
        void STDMETHODCALLTYPE CopyResource(ID3D11DeviceContext* context, ID3D11Resource* destination, ID3D11Resource* source)
        {
            ObserveCopy(context, destination, source, copyOriginal);
        }
        void STDMETHODCALLTYPE LiveCopyResource(ID3D11DeviceContext* context, ID3D11Resource* destination, ID3D11Resource* source)
        {
            ObserveCopy(context, destination, source, reinterpret_cast<Copy>(liveCopy.Original()));
        }
        std::string Owner(std::uintptr_t address)
        {
            HMODULE owner{};
            std::array<wchar_t, MAX_PATH> path{};
            if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &owner)) { GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size())); }
            return path[0] ? std::filesystem::path(path.data()).filename().string() : "unknown";
        }
        // Call after our last isolated context work and before producer calls
        // that must be observed, such as CS's HDR display composite.
        void EnsureContextObservers()
        {
            if (!producerContext) { return; }
            const bool dispatch = liveDispatch.Ensure(producerContext, 41, reinterpret_cast<std::uintptr_t>(&LiveDispatch));
            const bool copy = liveCopy.Ensure(producerContext, 47, reinterpret_cast<std::uintptr_t>(&LiveCopyResource));
            static bool reported{}, failed{};
            if (!reported || (!(dispatch && copy) && !failed)) {
                logger::info("[CS Adapter] renderer context=0x{:X} device=0x{:X} host=0x{:X}; live Dispatch={} ({}) CopyResource={} ({})",
                    reinterpret_cast<std::uintptr_t>(producerContext), reinterpret_cast<std::uintptr_t>(deviceContext),
                    reinterpret_cast<std::uintptr_t>(RenderPipeline::GetSingleton()->mContext),
                    dispatch ? "observed" : "FAILED", Owner(liveDispatch.Original()), copy ? "observed" : "FAILED", Owner(liveCopy.Original()));
                reported = true; failed = !(dispatch && copy);
            }
        }
        // The producer composes through the engine renderer's context.
        ID3D11DeviceContext* ProducerContext(RE::BSGraphics::Renderer* renderer)
        {
            producerContext = renderer ? reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context) : nullptr;
            EnsureContextObservers();
            return producerContext;
        }
        bool CompleteUI()
        {
            auto frame = Framebuffer();
            if (!frame) { return false; }
            NvidiaHost::GetSingleton()->CommunityFrame().SetUIBoundary(frame.Get());
            OverlayUI::GetSingleton()->OnPresent(frame.Get());
            // Our frame work is done; CS HDR composites before our Present hook.
            EnsureContextObservers();
            return true;
        }
        void Interface(std::int64_t arg)
        {
            // Chain all producer UI redirection and game drawing first. CS HDR
            // retains its UI target until its later display composite, which can
            // run before our Present hook is reached.
            uiBoundary.DrawInterface([&] { interfaceOriginal(arg); }, CompleteUI);
        }
        void ReportDisplayWait()
        {
            // Throttled per world frame: which boundary an HDR frame is still
            // waiting for, and whether our observers see the producer's calls.
            static std::uint64_t frames{};
            if (++frames % 600) { return; }
            const std::string_view status = NvidiaHost::GetSingleton()->CommunityFrame().Status();
            if (status.starts_with("Waiting for CS display")) {
                logger::info("[CS Adapter] {}; observed dispatches={} copies={} presents={} reinstalls={}/{}", status,
                    observedDispatches.load(std::memory_order_relaxed), observedCopies.load(std::memory_order_relaxed),
                    observedPresents.load(std::memory_order_relaxed), liveDispatch.Installs(), liveCopy.Installs());
            }
        }
        HRESULT STDMETHODCALLTYPE TopPresent(IDXGISwapChain* chain, UINT interval, UINT flags)
        {
            const bool ours = chain == gameSwapChain;
            const bool test = (flags & DXGI_PRESENT_TEST) != 0;
            if (ours && !test) {
                // These direct-output routes only run in the non-CS backend.
                // Clear stale activity even when CS skipped its world callback.
                PerformanceTuning::GetSingleton()->BeginRouteFrame();
                observedPresents.fetch_add(1, std::memory_order_relaxed);
            }
            if (ours) {
                // UI normally completed at the interface boundary. Late drawing is
                // a fallback only when the UI is the game-facing framebuffer itself.
                auto frame = Framebuffer();
                uiBoundary.BeforePresent(test, D3D11FrameCopy::SameObject(frame.Get(),
                    NvidiaHost::GetSingleton()->GameFacingTexture()), CompleteUI);
            }
            const auto result = presentOriginal(chain, interval, flags);
            if (ours) { uiBoundary.AfterPresent(test); }
            return result;
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
        interfaceOriginal = reinterpret_cast<DrawInterface>(HookSafety::InstallEntryDetour(
            REL::RelocationID(79947, 82084).address(), reinterpret_cast<std::uintptr_t>(&Interface)));
        if (!interfaceOriginal) { util::report_and_fail("Could not preserve the Community Shaders interface draw chain."); }
        installed = true;
        logger::info("[CS Adapter] engine postprocessing chain installed; CS owns upscaling, jitter and render scale");
        logger::info("[CS Adapter] interface completion boundary installed; overlay and UI identity precede display composition");
    }
    void InstallDeviceHooks(ID3D11DeviceContext* context, IDXGISwapChain* chain)
    {
        InstallVTableHook(context, 41, &Dispatch, dispatchOriginal);
        InstallVTableHook(context, 47, &CopyResource, copyOriginal);
        InstallVTableHook(chain, 8, &TopPresent, presentOriginal);
        gameSwapChain = chain;
        deviceContext = context;
        if (!dispatchOriginal || !copyOriginal || !presentOriginal) {
            util::report_and_fail("Could not preserve the CS display/Present chain.");
        }
    }
}
