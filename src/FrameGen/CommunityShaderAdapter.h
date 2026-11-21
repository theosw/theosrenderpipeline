#pragma once

#include "CommunityShaderFrame.h"
#include "CommunityShaderLens.h"
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
        // Before the first menu is drawn: take the completed scene again with
        // what the engine and the producer drew on it after post-processing,
        // then lay a redirected lens overlay over the scene itself.
        void SceneBeforeMenus();
        // From the producer's lens-target decision (Bottled Shaders' Camera FX),
        // before it draws: where its lens overlay goes instead of the scene, or
        // null to leave it there. framebuffer is what the interface draws on now;
        // beforeMenus says the scene can still be read and written before any menu.
        ID3D11RenderTargetView* LensTarget(ID3D11RenderTargetView* scene, ID3D11RenderTargetView* framebuffer, bool beforeMenus);
        // What happened to the lens overlay last, for the menu.
        const char* LensRoute() const { return lensRoute_; }
        HRESULT CaptureDisplayTransform(ID3D11DeviceContext* context, UINT x, UINT y, UINT z,
            CommunityShaderFrame::Dispatch dispatch);
        bool ConfirmPresentationCopy(ID3D11Resource* source);
        // The interface has been drawn on texture.
        void InterfaceCompleted(ID3D11Texture2D* texture);
        bool Prepare(const D3D11_TEXTURE2D_DESC& presentation);
        void PresentCompleted(bool succeeded);
        void ResetAfterRetirement();
        bool Ready() const { return worldCompleted_ && resources_.Ready(); }
        FrameExtent RenderExtent() const { return resources_.RenderExtent(); }
        const char* Status() const { return status_; }

    private:
        bool EvaluateWorld(ID3D11Texture2D* color, FrameExtent colorExtent);
        bool RefreshCompletedWorld();
        void ComposeLens(bool beforeMenus);
        void RouteLens(const char* route);
        CommunityShaderFrame resources_;
        CommunityShaderLens lens_;
        const char* lensRoute_{"No lens overlay seen yet"};
        unsigned lensRoutesLogged_{};
        // A lens was once laid over the HUD: the layer stays off until restart.
        bool lensSuspended_{};
        bool lensFailureReported_{};
        SourceDLSSG::CameraHistory history_, candidate_;
        SourceDLSSG::NeuralOptions options_, evaluatedOptions_;
        sl::Constants camera_{};
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        bool cameraValid_{}, eligible_{}, reset_{}, worldBegun_{}, upscalingCompleted_{}, worldCompleted_{}, prepared_{};
        bool neuralBoundaryReported_{};
        bool sceneRefreshReported_{}, sceneRefreshFailureReported_{};
        // Last completed scene was CS HDR Display's extended-range FP16 target.
        bool extendedScene_{};
        const char* status_{"Waiting for a CS world frame"};
        std::string guideRefusalLogged_;
        unsigned guideRefusalsLogged_{};
    };
}
