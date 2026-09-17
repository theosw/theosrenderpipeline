#include <PCH.h>
#include "CommunityShaderAdapter.h"
#include "SourceDLSSGBackend.h"

namespace TheosRenderPipeline
{
    bool CommunityShaderAdapter::BeginWorld(const Input& input)
    {
        if (prepared_ || resources_.Frame() == input.frame) { return false; }
        worldBegun_ = upscalingCompleted_ = worldCompleted_ = prepared_ = cameraValid_ = false;
        context_ = input.context;
        eligible_ = input.worldEligible;
        reset_ = input.reset;
        status_ = "Capturing CS world guides";
        const auto captured = resources_.CaptureGuides(input.context, input.frame, input.motion,
            input.depth, input.render, input.output);
        if (captured != S_OK) { status_ = "CS world guides unavailable"; return false; }
        candidate_ = history_;
        cameraValid_ = SourceDLSSG::CaptureCameraCandidate(input.graphics, input.render.width,
            input.render.height, input.jitterX, input.jitterY, reset_, input.jittered, camera_, candidate_);
        options_ = SourceDLSSG::Backend::Get().NeuralConfiguration();
        options_.worldOnly = true;
        options_.tuning.uiCorrection = false;
        // Both placements precede tone mapping. Preserve the explicit HDR
        // reconstruction preference: a float texture does not establish the
        // producer's color space or justify changing the user's transfer rule.
        worldBegun_ = true;
        if (options_.beforeUpscaling && !EvaluateWorld(input.world, input.render)) {
            worldBegun_ = false; return false;
        }
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
        if (reset_) { camera_.reset = sl::eTrue; }
        return result;
    }

    bool CommunityShaderAdapter::AfterUpscaling(ID3D11Texture2D* world)
    {
        if (!worldBegun_ || upscalingCompleted_ || worldCompleted_) { return false; }
        if (!options_.beforeUpscaling && !EvaluateWorld(world, resources_.OutputExtent())) {
            worldBegun_ = false; return false;
        }
        upscalingCompleted_ = true;
        return true;
    }

    bool CommunityShaderAdapter::CompleteWorld(ID3D11Texture2D* scene)
    {
        if (!worldBegun_ || !upscalingCompleted_ || worldCompleted_) { return false; }
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
        auto* hudless = resources_.Hudless(presentation);
        if (!Ready() || !cameraValid_ || !hudless) {
            status_ = "Waiting for matching CS scene, camera and display conversion";
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
        status_ = "Waiting for a CS world frame";
    }
}
