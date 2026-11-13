#pragma once

#include "SourceDLSSGInterop.h"
#include "UpscalerInputResolution.h"
#include "../../extern/fidelityfx-api/Kits/FidelityFX/upscalers/include/ffx_upscale.h"
#include "../../extern/fidelityfx-api/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"
#include <filesystem>
#include <chrono>
#include <string>
#include <vector>

namespace TheosRenderPipeline
{
    struct FSRCamera
    {
        float nearPlane{5.0f}, farPlane{100000.0f}, verticalFov{1.04719755f};
        float frameTimeMs{16.6667f};
        bool invertedDepth{};
    };

    // AMD's official FSR SDK 2.4 DX12 loader, on the same fenced transport as XeSS.
    // AMD picks FSR 4 (ML) on supported Radeon GPUs and FSR 3.1 elsewhere;
    // PreferClassic(true) asks for FSR 3.1 everywhere.
    // The host must prove retirement before releasing even a partial context.
    class FSRUpscaler final
    {
    public:
        FSRUpscaler() = default;
        FSRUpscaler(const FSRUpscaler&) = delete;
        FSRUpscaler& operator=(const FSRUpscaler&) = delete;
        HRESULT Open(ID3D12Device* device, const std::filesystem::path& directory);
        static bool RuntimePresent();
        // Takes effect at the next context creation.
        void PreferClassic(bool classic) { preferClassic_ = classic; }
        bool PrefersClassic() const { return preferClassic_; }
        // Provider names the runtime offered for this device, newest first; empty before Open.
        const std::vector<std::string>& AvailableVersions() const { return versionNames_; }
        static HRESULT QuerySize(UINT width, UINT height, int quality, FrameExtent& input);
        static HRESULT QueryInputRange(UINT width, UINT height, int quality, UpscalerInputResolution& range);
        static bool Accepts(FrameExtent input, FrameExtent output);
        HRESULT Initialize(SourceDLSSG::Interop& transport, ID3D11Device* device,
            FrameExtent output, int quality, DXGI_FORMAT format, bool invertedDepth = false,
            FrameExtent fixedInput = {});
        HRESULT Evaluate(ID3D11DeviceContext* context, ID3D11Texture2D* color,
            ID3D11Texture2D* motion, ID3D11Texture2D* depth, ID3D11Texture2D* destination,
            float jitterX, float jitterY, bool reset, float sharpness, const FSRCamera& camera);
        HRESULT ReleaseAfterRetirement();
        FrameExtent InputSize() const { return input_; }
        bool Ready() const { return initialized_; }
        const std::string& Status() const { return status_; }
        const std::string& Version() const { return version_; }
        std::uint64_t Frames() const { return frames_; }
    private:
        HRESULT Result(ffxReturnCode_t result, const char* operation);
        HRESULT CreateContext(bool invertedDepth);
        void QueryVersions();
        HMODULE module_{}; // Retained for process lifetime, including on failure.
        PfnFfxCreateContext create_{};
        PfnFfxDestroyContext destroy_{};
        PfnFfxDispatch dispatch_{};
        PfnFfxQuery query_{};
        ffxContext context_{};
        // API descriptor pointers remain live until destruction, as AMD requires.
        ffxCreateContextDescUpscale description_{};
        ffxCreateBackendDX12Desc backend_{};
        ffxCreateContextDescUpscaleVersion apiVersion_{};
        ffxOverrideVersion override_{};
        std::vector<std::uint64_t> versionIds_;
        std::vector<std::string> versionNames_;
        bool preferClassic_{};
        SourceDLSSG::Interop* transport_{};
        Microsoft::WRL::ComPtr<ID3D12Device> device12_;
        SourceDLSSG::SharedTexture color_, motion_, depth_, output_;
        D3D11FrameCopy::Depth depthCopy_;
        std::filesystem::path directory_;
        FrameExtent input_{}, outputSize_{};
        DXGI_FORMAT format_{};
        bool initialized_{}, invertedDepth_{};
        std::uint64_t frames_{};
        std::chrono::steady_clock::time_point lastFrame_{};
        std::string status_{"not initialized"}, version_{"unknown"};
    };
}
