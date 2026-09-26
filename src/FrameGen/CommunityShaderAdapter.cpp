#include <PCH.h>
#include "CommunityShaderAdapter.h"
#include "SourceDLSSGBackend.h"
#include "ReShadeIntegration.h"
#include "RenderPipeline.h"

namespace TheosRenderPipeline
{
    bool CommunityShaderAdapter::BeginWorld(const Input& input)
    {
        if (prepared_ || resources_.Frame() == input.frame) { return false; }
        worldBegun_ = upscalingCompleted_ = worldCompleted_ = prepared_ = cameraValid_ = false;
        context_ = input.context;
        resources_.SetProducerContext(input.producerContext);
        eligible_ = input.worldEligible;
        reset_ = input.reset;
        status_ = "Capturing CS world guides";
        const auto captured = resources_.CaptureGuides(input.context, input.frame, input.motion,
            input.depth, input.render, input.output);
        if (captured != S_OK) { status_ = "CS world guides unavailable"; return false; }
        candidate_ = history_;
        cameraValid_ = SourceDLSSG::CaptureCameraCandidate(input.graphics, input.render.width,
            input.render.height, input.jitterX, input.jitterY, reset_, input.jittered, camera_, candidate_);
        auto options = SourceDLSSG::Backend::Get().NeuralConfiguration();
        options.worldOnly = true;
        options.tuning.uiCorrection = false;
        options.reconstruction.producerColor = options.beforeUpscaling;
        // Late NR follows the completed scene's range, retained from the last
        // frame and corrected at CompleteWorld if CS HDR changed in between.
        if (!options.beforeUpscaling) {
            options.reconstruction = NeuralRendering::CompletedSceneContract(options.reconstruction, extendedScene_);
        }
        if (options != options_) { neuralBoundaryReported_ = false; }
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
        const bool result = SourceDLSSG::Backend::Get().EvaluateNeuralWorld(options_,
            cameraValid_ ? &camera_ : nullptr, eligible_, color, resources_.Motion(), resources_.Depth(),
            resources_.RenderExtent(), extent, reset_);
        if (!result) { status_ = "CS world NR stage failed"; }
        if (result && options_.enabled && eligible_ && cameraValid_ && color && !neuralBoundaryReported_) {
            D3D11_TEXTURE2D_DESC desc{}; color->GetDesc(&desc);
            logger::info("[CS Adapter] NR input={} allocation={}x{} active={}x{} format={} passes={} inputScale={} resolve={} HDR={} producerColor={}; UI excluded",
                options_.beforeUpscaling ? "pre-upscale world" : "post-processing scene",
                desc.Width, desc.Height, extent.width, extent.height, static_cast<unsigned>(desc.Format),
                options_.passes, options_.reconstruction.inputScale, static_cast<unsigned>(options_.reconstruction.method),
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
            if (!options_.beforeUpscaling) {
                auto options = SourceDLSSG::Backend::Get().NeuralConfiguration();
                options_.reconstruction = NeuralRendering::CompletedSceneContract(options.reconstruction, extended);
                neuralBoundaryReported_ = false;
            }
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
        worldBegun_ = upscalingCompleted_ = worldCompleted_ = prepared_ = cameraValid_ = false;
    }

    void CommunityShaderAdapter::ResetAfterRetirement()
    {
        resources_.ResetAfterRetirement(); context_.Reset();
        history_.Reset(); candidate_.Reset();
        worldBegun_ = upscalingCompleted_ = worldCompleted_ = prepared_ = cameraValid_ = false;
        neuralBoundaryReported_ = extendedScene_ = false;
        status_ = "Waiting for a CS world frame";
    }
}
