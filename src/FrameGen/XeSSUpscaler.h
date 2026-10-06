#pragma once

#include "SourceDLSSGInterop.h"
#include "../RCASParameters.h"
#include <xess/xess_d3d12.h>
#include <filesystem>
#include <string>

namespace TheosRenderPipeline
{
    // Uses the presenting queue and the host's existing cross-API fences.
    // Destruction/reinitialization requires retirement, just like the presenter.
    class XeSSUpscaler final
    {
    public:
        XeSSUpscaler() = default;
        XeSSUpscaler(const XeSSUpscaler&) = delete;
        XeSSUpscaler& operator=(const XeSSUpscaler&) = delete;
        HRESULT Open(ID3D12Device* device, const std::filesystem::path& directory);
        HRESULT QuerySize(UINT width, UINT height, int quality, FrameExtent& input);
        HRESULT Initialize(SourceDLSSG::Interop& transport, ID3D11Device* device,
            FrameExtent output, int quality, DXGI_FORMAT format, bool invertedDepth = true);
        HRESULT Evaluate(ID3D11DeviceContext* context, ID3D11Texture2D* color,
            ID3D11Texture2D* motion, ID3D11Texture2D* depth, ID3D11Texture2D* destination,
            float jitterX, float jitterY, bool reset, bool invertedDepth, float sharpness);
        HRESULT ReleaseAfterRetirement();
        FrameExtent InputSize() const { return input_; }
        bool Ready() const { return initialized_; }
        const std::string& Status() const { return status_; }
        std::uint64_t Frames() const { return frames_; }
        static xess_quality_settings_t Quality(int quality);
    private:
        HRESULT Result(xess_result_t result, const char* operation);
        HRESULT Sharpen(ID3D11DeviceContext* context, float strength);
        static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
        HMODULE module_{}; // Retained for process lifetime; SDK work can outlive a failed host.
        xess_context_handle_t context_{};
#define XESS_FUNCTION(name) decltype(&name) name##_{};
        XESS_FUNCTION(xessD3D12CreateContext)
        XESS_FUNCTION(xessD3D12Init)
        XESS_FUNCTION(xessD3D12Execute)
        XESS_FUNCTION(xessGetInputResolution)
        XESS_FUNCTION(xessSetVelocityScale)
        XESS_FUNCTION(xessDestroyContext)
#undef XESS_FUNCTION
        SourceDLSSG::Interop* transport_{};
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        SourceDLSSG::SharedTexture color_, motion_, depth_, output_;
        D3D11FrameCopy::Depth depthCopy_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharpened_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outputSRV_;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> sharpenUAV_;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> sharpenShader_;
        RCASParameters sharpenParameters_;
        std::filesystem::path directory_;
        FrameExtent input_{}, outputSize_{};
        int quality_{};
        bool initialized_{}, invertedDepth_{};
        std::uint64_t frames_{};
        std::string status_{"not initialized"};
    };
}
