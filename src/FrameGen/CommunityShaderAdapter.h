#pragma once

#include "CommunityShaderFrame.h"
#include "SourceDLSSGCamera.h"
#include "SourceDLSSGNeuralState.h"

namespace TheosRenderPipeline
{
    // Frame adapter for a producer that owns world shading, temporal jitter,
    // upscaling and UI. Engine/API hooks supply the boundaries; this owner has
    // no dependency on Community Shaders' private C++ object layout.
    class CommunityShaderAdapter
    {
    public:
        struct Input
        {
            ID3D11DeviceContext* context{};
            // The engine renderer's context, through which the producer composes.
            ID3D11DeviceContext* producerContext{};
            BSGraphics::State* graphics{};
            ID3D11Texture2D* world{};
            ID3D11Texture2D* motion{};
            ID3D11Texture2D* depth{};
            FrameExtent render{}, output{};
            std::uint64_t frame{};
            float jitterX{}, jitterY{};
            bool jittered{}, reset{}, worldEligible{};
        };

        bool BeginWorld(const Input& input);
        bool AfterUpscaling();
        bool CompleteWorld(ID3D11Texture2D* scene);
        HRESULT CaptureDisplayTransform(ID3D11DeviceContext* context, UINT x, UINT y, UINT z,
            CommunityShaderFrame::Dispatch dispatch);
        bool ConfirmPresentationCopy(ID3D11Resource* source);
        void SetUIBoundary(ID3D11Texture2D* texture) { resources_.SetUIBoundary(texture); }
        bool Prepare(ID3D11Texture2D* presentation);
        void PresentCompleted(bool succeeded);
        void ResetAfterRetirement();
        bool Ready() const { return worldCompleted_ && resources_.Ready(); }
        FrameExtent RenderExtent() const { return resources_.RenderExtent(); }
        const char* Status() const { return status_; }

    private:
        bool EvaluateWorld(ID3D11Texture2D* color, FrameExtent colorExtent);
        CommunityShaderFrame resources_;
        SourceDLSSG::CameraHistory history_, candidate_;
        SourceDLSSG::NeuralOptions options_, evaluatedOptions_;
        sl::Constants camera_{};
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        bool cameraValid_{}, eligible_{}, reset_{}, worldBegun_{}, upscalingCompleted_{}, worldCompleted_{}, prepared_{};
        bool neuralBoundaryReported_{};
        // Last completed scene was CS HDR Display's extended-range FP16 target.
        bool extendedScene_{};
        bool uiCaptureReported_{}, uiCaptureFailed_{};
        const char* status_{"Waiting for a CS world frame"};
    };
}
