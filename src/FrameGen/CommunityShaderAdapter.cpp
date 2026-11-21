#include <PCH.h>
#include "CommunityShaderAdapter.h"
#include "SourceDLSSGBackend.h"
#include "ReShadeIntegration.h"
#include "RenderPipeline.h"
#include "NeuralCombatMode.h"
#include "WeatherAppearanceRuntime.h"

namespace TheosRenderPipeline
{
    bool CommunityShaderAdapter::BeginWorld(const Input& input)
    {
        if (prepared_ || resources_.Frame() == input.frame) { return false; }
        lens_.NextFrame();
        worldBegun_ = upscalingCompleted_ = worldCompleted_ = prepared_ = cameraValid_ = false;
        context_ = input.context;
        resources_.SetProducerContext(input.producerContext);
        eligible_ = input.worldEligible;
        reset_ = input.reset;
        status_ = "Capturing CS world guides";
        const auto captured = resources_.CaptureGuides(input.context, input.frame, input.motion,
            input.depth, input.render, input.output);
        if (captured != S_OK) {
            status_ = "CS world guides unavailable";
            // Each distinct refusal once, and a few at most: this runs every frame.
            if (FAILED(captured) && guideRefusalsLogged_ < 8 && resources_.GuideRefusal() != guideRefusalLogged_) {
                guideRefusalLogged_ = resources_.GuideRefusal();
                ++guideRefusalsLogged_;
                logger::error("[CS Adapter] world guides refused: {}", guideRefusalLogged_);
            }
            return false;
        }
        candidate_ = history_;
        cameraValid_ = SourceDLSSG::CaptureCameraCandidate(input.graphics, input.render.width,
            input.render.height, input.jitterX, input.jitterY, reset_, input.jittered, camera_, candidate_);
        auto options = SourceDLSSG::Backend::Get().NeuralConfiguration();
        options.worldOnly = true;
        // CS owns its sharpening. Only TRP NR settings are applied on this route.
        bool sharpening = false;
        float sharpness = RenderPipeline::GetSingleton()->mSharpness;
        Appearance::Runtime::Get().Apply(options, sharpening, sharpness);
        // Route contract after presets, which may change placement or UI correction.
        options.tuning.uiCorrection = false;
        options.reconstruction.producerColor = options.beforeUpscaling;
        // Apply appearance to saved preferences before transient combat overrides.
#if !defined(TRP_NO_NEURAL_RENDERING)
        NeuralRendering::ApplyCombatMode(options, eligible_);
#endif
        // Keep the resolved preset until the actual completed scene is known.
        // Re-reading global preferences at an HDR transition loses this frame's
        // appearance overrides; using the previous format can miss a transition.
        options_ = std::move(options);
        // Early CS color is unfinished producer RGB, not a display-ready image.
        // The paired proxy transfers only NR's changes back to the retained scene.
        // Late NR keeps the user's completed-scene reconstruction configuration.
        worldBegun_ = true;
        if (options_.beforeUpscaling && !EvaluateWorld(input.world, input.render)) {
            worldBegun_ = false; return false;
        }
        auto& effects = ReShadeIntegration::Get();
        effects.SetBeforeUpscaling(RenderPipeline::GetSingleton()->mReShadeBeforeUpscaling);
        effects.Render(input.world, eligible_ ? resources_.Depth() : nullptr, input.render, input.render, true);
        return true;
    }

    bool CommunityShaderAdapter::EvaluateWorld(ID3D11Texture2D* color, FrameExtent extent)
    {
        D3D11ContextIsolation::Scope scope{resources_.Isolation(), context_.Get()};
        if (!scope) { status_ = "CS context unavailable at NR boundary"; return false; }
        if (!SourceDLSSG::SameHistoryOptions(options_, evaluatedOptions_)) { neuralBoundaryReported_ = false; }
        evaluatedOptions_ = options_;
        const bool result = SourceDLSSG::Backend::Get().EvaluateNeuralWorld(options_,
            cameraValid_ ? &camera_ : nullptr, eligible_, color, resources_.Motion(), resources_.Depth(),
            resources_.RenderExtent(), extent, reset_);
        if (!result) { status_ = "CS world NR stage failed"; }
        if (result && options_.enabled && eligible_ && cameraValid_ && color && !neuralBoundaryReported_) {
            D3D11_TEXTURE2D_DESC desc{}; color->GetDesc(&desc);
            logger::info("[CS Adapter] NR input={} allocation={}x{} active={}x{} format={} passes={} inputScale={} resolve={} HDR={} producerColor={}; UI excluded",
                options_.beforeUpscaling ? "pre-upscale world" : "post-processing scene",
                desc.Width, desc.Height, extent.width, extent.height, static_cast<unsigned>(desc.Format),
                options_.EffectivePasses(), options_.reconstruction.inputScale, static_cast<unsigned>(options_.reconstruction.method),
                options_.reconstruction.colorIsHDR, options_.reconstruction.producerColor);
            neuralBoundaryReported_ = true;
        }
        if (reset_) { camera_.reset = sl::eTrue; }
        return result;
    }

    bool CommunityShaderAdapter::AfterUpscaling()
    {
        if (!worldBegun_ || upscalingCompleted_ || worldCompleted_) { return false; }
        // This boundary is before engine post-processing. kMAIN still contains
        // unfinished scene color here, even though CS has already upscaled it.
        upscalingCompleted_ = true;
        return true;
    }

    bool CommunityShaderAdapter::CompleteWorld(ID3D11Texture2D* scene)
    {
        if (!worldBegun_ || !upscalingCompleted_ || worldCompleted_) { return false; }
        // The engine call has returned, but CS has not restored its framebuffer
        // redirection or entered UI rendering. Evaluate on that completed scene,
        // then snapshot the corrected pixels for frame generation. In particular,
        // do not feed direct NR output back through CS's exposure and tone mapping.
        D3D11_TEXTURE2D_DESC sceneDesc{};
        if (scene) { scene->GetDesc(&sceneDesc); }
        // CS HDR Display redirects the completed scene to FP16 only while HDR
        // output is enabled; its SDR scene keeps the presentation format.
        const bool extended = sceneDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (extended != extendedScene_) {
            extendedScene_ = extended;
            logger::info("[CS Adapter] completed scene format={} {}; late NR uses {} colour", static_cast<unsigned>(sceneDesc.Format),
                extended ? "extended-range HDR" : "display-range", extended ? "producer-restored" : "the saved");
        }
        if (!options_.beforeUpscaling) {
            options_.reconstruction = NeuralRendering::CompletedSceneContract(options_.reconstruction, extended);
        }
        if (!options_.beforeUpscaling && !EvaluateWorld(scene, resources_.OutputExtent())) {
            worldBegun_ = false; return false;
        }
        ReShadeIntegration::Get().Render(scene, eligible_ ? resources_.Depth() : nullptr,
            resources_.OutputExtent(), resources_.RenderExtent(), false);
        worldCompleted_ = SUCCEEDED(resources_.CaptureScene(context_.Get(), scene));
        status_ = worldCompleted_ ? "CS world captured before UI" : "CS completed world unavailable";
        return worldCompleted_;
    }

    void CommunityShaderAdapter::RouteLens(const char* route)
    {
        if (lensRoute_ == route) { return; }
        lensRoute_ = route;
        // Routes alternate with menus and weather; the first changes tell the story.
        if (lensRoutesLogged_ < 24) { ++lensRoutesLogged_; logger::info("[CS Adapter] Camera FX lens overlay: {}", route); }
    }

    ID3D11RenderTargetView* CommunityShaderAdapter::LensTarget(ID3D11RenderTargetView* scene,
        ID3D11RenderTargetView* framebuffer, bool beforeMenus)
    {
        auto& backend = SourceDLSSG::Backend::Get();
        auto* open = worldCompleted_ && !prepared_ ? resources_.OpenScene() : nullptr;
        const auto route = ChooseLensRoute(backend.CommunityLens(), backend.Enabled(), lensSuspended_, beforeMenus, open,
            CommunityShaderFrame::Texture(scene).Get(), CommunityShaderFrame::Texture(framebuffer).Get(), resources_.UISurface());
        if (route == LensRoute::ProducerSurface) { RouteLens(Describe(route)); return framebuffer; }
        if (route != LensRoute::Layer) { RouteLens(Describe(route)); return nullptr; }
        if (auto* layer = lens_.Begin(context_.Get(), open)) { RouteLens(Describe(route)); return layer; }
        using Refusal = CommunityShaderLens::Refusal;
        RouteLens(lens_.LastRefusal() == Refusal::Format ? "on the scene; the scene format cannot be layered" :
            lens_.LastRefusal() == Refusal::Resources ? "on the scene; the lens layer could not be created" :
            lens_.LastRefusal() == Refusal::Busy ? "on the scene; a lens layer was already drawn this frame" :
            "on the scene; the scene resource cannot be layered");
        return nullptr;
    }

    void CommunityShaderAdapter::ComposeLens(bool beforeMenus)
    {
        if (!lens_.Pending()) { return; }
        const auto result = lens_.Compose(context_.Get());
        if (FAILED(result) && !lensFailureReported_) {
            lensFailureReported_ = true;
            logger::error("[CS Adapter] Camera FX lens layer could not be laid over the scene (0x{:08X}); the lens is missing from this frame",
                static_cast<unsigned>(result));
        } else if (SUCCEEDED(result) && !beforeMenus && !lensSuspended_) {
            // Kept on the frame this once, in the wrong order. Whatever stopped
            // the world-completion call will stop it again: back to the scene.
            lensSuspended_ = true;
            logger::warn("[CS Adapter] Camera FX lens layer was laid over the frame after the interface, above the HUD, because "
                "the engine's world-completion call did not arrive; lens effects go back on the scene until restart");
        }
    }

    void CommunityShaderAdapter::SceneBeforeMenus()
    {
        // The HUD-less scene first: it must not hold the lens layer.
        RefreshCompletedWorld();
        ComposeLens(true);
    }

    void CommunityShaderAdapter::InterfaceCompleted(ID3D11Texture2D* texture)
    {
        resources_.SetUIBoundary(texture);
        // Never lose a redirected lens: late is above the HUD, but on the frame.
        ComposeLens(false);
    }

    bool CommunityShaderAdapter::RefreshCompletedWorld()
    {
        if (!worldCompleted_ || prepared_) { return false; }
        // NR and ReShade already ran on the scene in CompleteWorld; this only
        // adds what was drawn on it since. It stays HUD-less: no menu has been
        // drawn, and a redirected lens overlay is not on the scene yet.
        const auto result = resources_.RefreshScene(context_.Get());
        if (result == S_OK && !sceneRefreshReported_) {
            sceneRefreshReported_ = true;
            logger::info("[CS Adapter] HUD-less scene taken again at the interface boundary; it includes what was drawn on the scene after post-processing");
        } else if (FAILED(result) && !sceneRefreshFailureReported_) {
            sceneRefreshFailureReported_ = true;
            logger::warn("[CS Adapter] HUD-less scene could not be taken again at the interface boundary (0x{:08X}); keeping the post-processing snapshot",
                static_cast<unsigned>(result));
        }
        return result == S_OK;
    }

    HRESULT CommunityShaderAdapter::CaptureDisplayTransform(ID3D11DeviceContext* context,
        UINT x, UINT y, UINT z, CommunityShaderFrame::Dispatch dispatch)
    {
        if (!worldCompleted_) { return S_FALSE; }
        return resources_.CaptureDisplayTransform(context, x, y, z, dispatch);
    }

    bool CommunityShaderAdapter::ConfirmPresentationCopy(ID3D11Resource* source)
    {
        return worldCompleted_ && resources_.ConfirmPresentationCopy(source);
    }

    bool CommunityShaderAdapter::Prepare(const D3D11_TEXTURE2D_DESC& presentation)
    {
        if (prepared_) { return false; }
        if (!Ready()) { return false; } // Keep the failed world-stage diagnostic.
        if (!cameraValid_) { status_ = "Waiting for CS camera data"; return false; }
        auto* hudless = resources_.Hudless(presentation);
        if (!hudless) {
            using State = CommunityShaderFrame::PresentationState;
            switch (resources_.PresentationStatus(presentation)) {
            case State::AwaitingTransform: status_ = "Waiting for CS display conversion after UI"; break;
            case State::AwaitingCopy: status_ = "Waiting for CS display copy to the presentation buffer"; break;
            case State::ExtentMismatch: status_ = "CS scene and presentation sizes differ"; break;
            case State::FormatMismatch: status_ = "CS scene and presentation formats differ"; break;
            default: status_ = "Waiting for a completed CS scene"; break;
            }
            return false;
        }
        D3D11ContextIsolation::Scope scope{resources_.Isolation(), context_.Get()};
        if (!scope) { status_ = "CS context unavailable at submission"; return false; }
        const auto render = resources_.RenderExtent(), output = resources_.OutputExtent();
        // No UI layer: the HUD is not in one on this route, and DLSS-G's UI
        // layer must hold the whole interface. A lens layer alone is not one.
        prepared_ = SourceDLSSG::Backend::Get().Prepare(camera_, resources_.Motion(), resources_.Depth(),
            nullptr, hudless, render, output.width, output.height, eligible_);
        status_ = prepared_ ? "CS frame submitted to NVIDIA" : "CS frame preparation failed";
        return prepared_;
    }

    void CommunityShaderAdapter::PresentCompleted(bool succeeded)
    {
        // A suppressed Present must never enter this boundary. Snapshot once,
        // commit once, and never re-read the producer's restored camera state.
        if (prepared_ && succeeded) { history_ = candidate_; }
        else { history_.Reset(); }
        resources_.Consume();
        lens_.NextFrame();
        worldBegun_ = upscalingCompleted_ = worldCompleted_ = prepared_ = cameraValid_ = false;
    }

    void CommunityShaderAdapter::ResetAfterRetirement()
    {
        resources_.ResetAfterRetirement(); lens_.ResetAfterRetirement(); context_.Reset();
        history_.Reset(); candidate_.Reset();
        worldBegun_ = upscalingCompleted_ = worldCompleted_ = prepared_ = cameraValid_ = false;
        neuralBoundaryReported_ = extendedScene_ = false;
        sceneRefreshReported_ = sceneRefreshFailureReported_ = false;
        lensFailureReported_ = false;
        status_ = "Waiting for a CS world frame";
    }
}
