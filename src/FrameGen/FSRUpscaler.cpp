#include "FSRUpscaler.h"
#include "../PluginPaths.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace TheosRenderPipeline
{
    HRESULT FSRUpscaler::Result(ffxReturnCode_t result, const char* operation)
    {
        if (result == FFX_API_RETURN_OK) { return S_OK; }
        status_ = std::string(operation) + " FSR result=" + std::to_string(result);
        return result == FFX_API_RETURN_ERROR_MEMORY ? E_OUTOFMEMORY : E_FAIL;
    }

    HRESULT FSRUpscaler::Open(ID3D12Device* device, const std::filesystem::path& directory)
    {
        if (!device || !directory.is_absolute()) { return E_INVALIDARG; }
        if (module_) {
            if (!PluginPaths::EqualPath(directory_, directory) || !create_ || !destroy_ || !dispatch_ || !query_) { return E_UNEXPECTED; }
            if (context_ && device12_.Get() != device) { return E_UNEXPECTED; }
            // A retired context may be recreated on a new device in this process.
            device12_ = device; return S_OK;
        }
        directory_ = PluginPaths::Normalize(directory);
        if (GetModuleHandleW(L"amd_fidelityfx_dx12.dll")) {
            status_ = "another owner loaded amd_fidelityfx_dx12.dll"; return E_UNEXPECTED;
        }
        module_ = LoadLibraryExW((directory_ / L"amd_fidelityfx_dx12.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module_) { status_ = "AMD/amd_fidelityfx_dx12.dll missing or not loadable; reinstall TRP Standard"; return HRESULT_FROM_WIN32(GetLastError()); }
        device12_ = device;
        create_ = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module_, "ffxCreateContext"));
        destroy_ = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module_, "ffxDestroyContext"));
        dispatch_ = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module_, "ffxDispatch"));
        query_ = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module_, "ffxQuery"));
        if (!create_ || !destroy_ || !dispatch_ || !query_) { status_ = "missing FidelityFX API exports"; return E_NOINTERFACE; }
        status_ = "FSR runtime loaded"; return S_OK;
    }

    bool FSRUpscaler::RuntimePresent()
    {
        std::error_code error;
        return std::filesystem::is_regular_file(PluginPaths::AMDDirectory() / L"amd_fidelityfx_dx12.dll", error);
    }

    HRESULT FSRUpscaler::QueryInputRange(UINT width, UINT height, int quality, UpscalerInputResolution& range)
    {
        range = {};
        FrameExtent optimal{};
        const auto hr = QuerySize(width, height, quality, optimal);
        if (FAILED(hr)) { return hr; }
        // Fixed render extents are supplied when creating the FSR context.
        // Quality determines the preferred ratio, not a narrower SDK range.
        range = {optimal, {1, 1}, {width, height}};
        return S_OK;
    }

    HRESULT FSRUpscaler::QuerySize(UINT width, UINT height, int quality, FrameExtent& input)
    {
        if (!width || !height || quality < 0 || quality > 4) { return E_INVALIDARG; }
        // Match the existing quality index. Ultra Quality is a custom 1.3x ratio;
        // AMD exposes arbitrary fixed render extents as well as its named modes.
        constexpr std::array ratios{2.0, 1.7, 1.5, 3.0, 1.3};
        input = {std::max(1u, static_cast<UINT>(width / ratios[quality])),
                 std::max(1u, static_cast<UINT>(height / ratios[quality]))};
        return S_OK;
    }

    bool FSRUpscaler::Accepts(FrameExtent input, FrameExtent output)
    {
        return UpscalerInputResolution{output, {1, 1}, output}.Accepts(input, output);
    }

    HRESULT FSRUpscaler::CreateContext(bool invertedDepth)
    {
        backend_ = {}; backend_.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend_.device = device12_.Get();
        description_ = {}; description_.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &backend_.header};
        // TRP's SDR world is already tone mapped by Skyrim/ENB. Exposure is 1;
        // native UI and TRP HDR output are composed after temporal reconstruction.
        description_.flags = invertedDepth ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0;
        if (format_ == DXGI_FORMAT_R8G8B8A8_UNORM) { description_.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE; }
        else { description_.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE; }
        description_.maxRenderSize = {input_.width, input_.height};
        description_.maxUpscaleSize = {outputSize_.width, outputSize_.height};
        const auto hr = Result(create_(&context_, &description_.header, nullptr), "create context");
        if (FAILED(hr)) { return hr; }
        invertedDepth_ = invertedDepth;
        ffxQueryGetProviderVersion version{}; version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        const auto queried = Result(query_(&context_, &version.header), "query provider version");
        if (FAILED(queried) || !version.versionName) { return FAILED(queried) ? queried : E_UNEXPECTED; }
        version_ = version.versionName;
        return S_OK;
    }

    HRESULT FSRUpscaler::Initialize(SourceDLSSG::Interop& transport, ID3D11Device* device,
        FrameExtent output, int quality, DXGI_FORMAT format, bool invertedDepth, FrameExtent fixedInput)
    {
        if (!module_ || !device12_ || !create_ || !destroy_ || !dispatch_ || !query_ || !device || context_ || initialized_) { return E_UNEXPECTED; }
        if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            status_ = "FSR requires RGBA8 or RGBA16F producer"; return DXGI_ERROR_UNSUPPORTED;
        }
        FrameExtent input{};
        auto hr = QuerySize(output.width, output.height, quality, input);
        if (FAILED(hr)) { return hr; }
        if (fixedInput.width || fixedInput.height) { input = fixedInput; }
        if (!Accepts(input, output)) { status_ = "invalid FSR render extent or aspect"; return E_INVALIDARG; }
        input_ = input; outputSize_ = output; format_ = format; transport_ = &transport;
        auto make = [&](SourceDLSSG::SharedTexture& texture, FrameExtent size, DXGI_FORMAT textureFormat, UINT bindings) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = size.width; desc.Height = size.height;
            desc.MipLevels = desc.ArraySize = 1; desc.Format = textureFormat;
            desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = bindings | D3D11_BIND_RENDER_TARGET;
            return transport.CreateSharedTexture(desc, texture);
        };
        if (FAILED(hr = make(color_, input, format, D3D11_BIND_SHADER_RESOURCE)) ||
            FAILED(hr = make(motion_, input, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE)) ||
            FAILED(hr = make(depth_, input, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) ||
            FAILED(hr = make(output_, output, format, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) ||
            FAILED(hr = CreateContext(invertedDepth))) { return hr; }
        initialized_ = true; frames_ = 0; status_ = "FSR " + version_ + " DX12 ready"; return S_OK;
    }

    HRESULT FSRUpscaler::Evaluate(ID3D11DeviceContext* context, ID3D11Texture2D* color,
        ID3D11Texture2D* motion, ID3D11Texture2D* depth, ID3D11Texture2D* destination,
        float jitterX, float jitterY, bool reset, float sharpness, const FSRCamera& camera)
    {
        if (!initialized_ || !context || !destination) { return E_UNEXPECTED; }
        D3D11_TEXTURE2D_DESC desc{}; destination->GetDesc(&desc);
        if (desc.Width != outputSize_.width || desc.Height != outputSize_.height || desc.Format != format_ ||
            !D3D11FrameCopy::ValidResources(context, output_.texture11.Get(), destination) ||
            !std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
            camera.nearPlane <= 0 || camera.farPlane <= camera.nearPlane ||
            !std::isfinite(camera.verticalFov) || camera.verticalFov <= 0 || camera.verticalFov >= 3.14159265f ||
            !std::isfinite(camera.frameTimeMs) || camera.frameTimeMs < 0) { return E_INVALIDARG; }
        HRESULT hr;
        if (camera.invertedDepth != invertedDepth_) {
            // No context or history may be destroyed while the GPU still reads it.
            if (FAILED(hr = transport_->SignalD3D11(SourceDLSSG::Work::Upscaling)) || FAILED(hr = transport_->Drain()) ||
                FAILED(hr = Result(destroy_(&context_, nullptr), "depth convention destruction"))) { return hr; }
            context_ = nullptr;
            if (FAILED(hr = CreateContext(camera.invertedDepth))) { initialized_ = false; return hr; }
            reset = true;
        }
        hr = transport_->CopyInputRegion(color, color_, input_);
        if (FAILED(hr) || FAILED(hr = transport_->CopyInputRegion(motion, motion_, input_)) ||
            FAILED(hr = depthCopy_.Copy(context, depth, depth_.texture11.Get(), input_)) ||
            FAILED(hr = transport_->SignalD3D11(SourceDLSSG::Work::Upscaling))) { return hr; }
        ID3D12GraphicsCommandList* list{};
        if (FAILED(hr = transport_->Begin(SourceDLSSG::Work::Upscaling, &list))) { return hr; }
        ffxDispatchDescUpscale execute{}; execute.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        execute.commandList = list;
        // The official backend transitions imports and restores their declared
        // state when unregistering. Both APIs own these resources in COMMON.
        execute.color = ffxApiGetResourceDX12(color_.texture12.Get(), FFX_API_RESOURCE_STATE_COMMON);
        execute.depth = ffxApiGetResourceDX12(depth_.texture12.Get(), FFX_API_RESOURCE_STATE_COMMON);
        execute.motionVectors = ffxApiGetResourceDX12(motion_.texture12.Get(), FFX_API_RESOURCE_STATE_COMMON);
        execute.output = ffxApiGetResourceDX12(output_.texture12.Get(), FFX_API_RESOURCE_STATE_COMMON);
        execute.jitterOffset = {jitterX, jitterY};
        execute.motionVectorScale = {static_cast<float>(input_.width), static_cast<float>(input_.height)};
        execute.renderSize = {input_.width, input_.height}; execute.upscaleSize = {outputSize_.width, outputSize_.height};
        execute.enableSharpening = sharpness > 0; execute.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
        const auto now = std::chrono::steady_clock::now();
        execute.frameTimeDelta = camera.frameTimeMs > 0 ? camera.frameTimeMs :
            (lastFrame_.time_since_epoch().count() ? std::chrono::duration<float, std::milli>(now - lastFrame_).count() : 16.6667f);
        execute.frameTimeDelta = std::clamp(execute.frameTimeDelta, 1.0f, 1000.0f);
        lastFrame_ = now; execute.preExposure = 1;
        execute.reset = reset || frames_ == 0;
        execute.cameraNear = camera.invertedDepth ? camera.farPlane : camera.nearPlane;
        execute.cameraFar = camera.invertedDepth ? camera.nearPlane : camera.farPlane;
        execute.cameraFovAngleVertical = camera.verticalFov; execute.viewSpaceToMetersFactor = 0.0142875f;
        if (format_ == DXGI_FORMAT_R8G8B8A8_UNORM) { execute.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB; }
        const auto execution = Result(dispatch_(&context_, &execute.header), "dispatch");
        // Fence even failed dispatches: partially recorded SDK work still owns memory.
        if (FAILED(hr = transport_->Submit(SourceDLSSG::Work::Upscaling)) ||
            FAILED(hr = transport_->WaitD3D11(SourceDLSSG::Work::Upscaling))) { return hr; }
        if (FAILED(execution)) { return execution; }
        context->CopyResource(destination, output_.texture11.Get());
        ++frames_; return S_OK;
    }

    HRESULT FSRUpscaler::ReleaseAfterRetirement()
    {
        if (context_) {
            const auto hr = Result(destroy_(&context_, nullptr), "destroy context");
            if (FAILED(hr)) { return hr; }
            context_ = nullptr;
        }
        color_ = {}; motion_ = {}; depth_ = {}; output_ = {}; depthCopy_.ResetViews();
        description_ = {}; backend_ = {}; // Keep the opened device for same-device reinitialization.
        initialized_ = false; transport_ = nullptr; frames_ = 0; lastFrame_ = {}; return S_OK;
    }
}
