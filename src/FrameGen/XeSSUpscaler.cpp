#include "XeSSUpscaler.h"
#include "../PluginPaths.h"
#include <array>
#include <algorithm>

namespace TheosRenderPipeline
{
    xess_quality_settings_t XeSSUpscaler::Quality(int quality)
    {
        constexpr std::array qualities{XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_BALANCED,
            XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE,
            XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_AA};
        return qualities[static_cast<std::size_t>(std::clamp(quality, 0, 5))];
    }
    HRESULT XeSSUpscaler::Result(xess_result_t result, const char* operation)
    {
        if (result < 0) {
            status_ = std::string(operation) + " XeSS result=" + std::to_string(result);
            return result == XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY ? E_OUTOFMEMORY : E_FAIL;
        }
        return S_OK;
    }
    HRESULT XeSSUpscaler::Open(ID3D12Device* device, const std::filesystem::path& directory)
    {
        if (!device || !directory.is_absolute()) { return E_INVALIDARG; }
        if (context_) { return PluginPaths::EqualPath(directory_, directory) ? S_OK : E_UNEXPECTED; }
        directory_ = PluginPaths::Normalize(directory);
        if (!module_) {
            if (GetModuleHandleW(L"libxess.dll")) { status_ = "another owner loaded libxess.dll"; return E_UNEXPECTED; }
            module_ = LoadLibraryExW((directory_ / L"libxess.dll").c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module_) { status_ = "load libxess.dll"; return HRESULT_FROM_WIN32(GetLastError()); }
        }
#define RESOLVE(name) name##_ = reinterpret_cast<decltype(name##_)>(GetProcAddress(module_, #name)); \
        if (!name##_) { status_ = "missing " #name; return E_NOINTERFACE; }
        RESOLVE(xessD3D12CreateContext)
        RESOLVE(xessD3D12Init)
        RESOLVE(xessD3D12Execute)
        RESOLVE(xessGetInputResolution)
        RESOLVE(xessSetVelocityScale)
        RESOLVE(xessDestroyContext)
#undef RESOLVE
        return Result(xessD3D12CreateContext_(device, &context_), "create context");
    }
    HRESULT XeSSUpscaler::QuerySize(UINT width, UINT height, int quality, FrameExtent& input)
    {
        if (!context_ || !width || !height || quality < 0 || quality > 5) { return E_INVALIDARG; }
        const xess_2d_t output{width, height}; xess_2d_t render{};
        const auto hr = Result(xessGetInputResolution_(context_, &output, Quality(quality), &render), "input resolution");
        if (FAILED(hr)) { return hr; }
        if (!render.x || !render.y || render.x > width || render.y > height) { return E_UNEXPECTED; }
        input = {render.x, render.y}; return S_OK;
    }
    HRESULT XeSSUpscaler::Initialize(SourceDLSSG::Interop& transport, ID3D11Device* device,
        FrameExtent output, int quality, DXGI_FORMAT format, bool invertedDepth)
    {
        if (!context_ || !device || initialized_) { return E_UNEXPECTED; }
        // Skyrim's SDR source is RGBA8. HDR output remains a later, separate stage.
        // Reject incompatible producers rather than reinterpret their pixels.
        if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            status_ = "XeSS requires RGBA8 or RGBA16F producer"; return DXGI_ERROR_UNSUPPORTED;
        }
        transport_ = &transport; device_ = device; outputSize_ = output; quality_ = quality; invertedDepth_ = invertedDepth;
        auto hr = QuerySize(output.width, output.height, quality, input_);
        if (FAILED(hr)) { return hr; }
        xess_d3d12_init_params_t init{};
        init.outputResolution = {output.width, output.height}; init.qualitySetting = Quality(quality);
        init.initFlags = (invertedDepth ? XESS_INIT_FLAG_INVERTED_DEPTH : 0) |
            (format == DXGI_FORMAT_R8G8B8A8_UNORM ? XESS_INIT_FLAG_LDR_INPUT_COLOR : 0);
        if (FAILED(hr = Result(xessD3D12Init_(context_, &init), "initialize")) ||
            FAILED(hr = Result(xessSetVelocityScale_(context_, static_cast<float>(input_.width),
                static_cast<float>(input_.height)), "velocity scale"))) { return hr; }
        auto make = [&](SourceDLSSG::SharedTexture& texture, FrameExtent size, DXGI_FORMAT textureFormat, UINT bindings) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = size.width; desc.Height = size.height;
            desc.MipLevels = desc.ArraySize = 1; desc.Format = textureFormat;
            desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bindings;
            return transport.CreateSharedTexture(desc, texture);
        };
        if (FAILED(hr = make(color_, input_, format, D3D11_BIND_SHADER_RESOURCE)) ||
            FAILED(hr = make(motion_, input_, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE)) ||
            FAILED(hr = make(depth_, input_, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) ||
            FAILED(hr = make(output_, output, format, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS))) { return hr; }
        auto desc = output_.desc; desc.MiscFlags = 0;
        if (FAILED(hr = device_->CreateTexture2D(&desc, nullptr, &sharpened_)) ||
            FAILED(hr = device_->CreateShaderResourceView(output_.texture11.Get(), nullptr, &outputSRV_)) ||
            FAILED(hr = device_->CreateUnorderedAccessView(sharpened_.Get(), nullptr, &sharpenUAV_))) { return hr; }
        initialized_ = true; status_ = "XeSS D3D12 ready"; return S_OK;
    }
    void XeSSUpscaler::Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &barrier);
    }
    HRESULT XeSSUpscaler::Evaluate(ID3D11DeviceContext* context, ID3D11Texture2D* color,
        ID3D11Texture2D* motion, ID3D11Texture2D* depth, ID3D11Texture2D* destination,
        float jitterX, float jitterY, bool reset, bool invertedDepth, float sharpness)
    {
        if (!initialized_ || !context || !destination) { return E_UNEXPECTED; }
        D3D11_TEXTURE2D_DESC destinationDesc{}; destination->GetDesc(&destinationDesc);
        if (destinationDesc.Width != outputSize_.width || destinationDesc.Height != outputSize_.height ||
            destinationDesc.Format != output_.desc.Format ||
            !D3D11FrameCopy::ValidResources(context, output_.texture11.Get(), destination)) { return E_INVALIDARG; }
        if (invertedDepth != invertedDepth_) {
            // A depth convention change is rare; the SDK requires retirement before reinit.
            auto hr = transport_->SignalD3D11(SourceDLSSG::Work::Upscaling);
            if (FAILED(hr) || FAILED(hr = transport_->Drain())) { return hr; }
            xess_d3d12_init_params_t init{}; init.outputResolution = {outputSize_.width, outputSize_.height};
            init.qualitySetting = Quality(quality_); init.initFlags = (invertedDepth ? XESS_INIT_FLAG_INVERTED_DEPTH : 0) |
                (output_.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? XESS_INIT_FLAG_LDR_INPUT_COLOR : 0);
            if (FAILED(hr = Result(xessD3D12Init_(context_, &init), "depth convention reinit"))) { return hr; }
            invertedDepth_ = invertedDepth; reset = true;
            if (FAILED(hr = Result(xessSetVelocityScale_(context_, static_cast<float>(input_.width),
                static_cast<float>(input_.height)), "velocity scale"))) { return hr; }
        }
        auto hr = transport_->CopyInputRegion(color, color_, input_);
        if (FAILED(hr) || FAILED(hr = transport_->CopyInputRegion(motion, motion_, input_)) ||
            FAILED(hr = depthCopy_.Copy(context, depth, depth_.texture11.Get(), input_)) ||
            FAILED(hr = transport_->SignalD3D11(SourceDLSSG::Work::Upscaling))) { return hr; }
        ID3D12GraphicsCommandList* list{};
        if (FAILED(hr = transport_->Begin(SourceDLSSG::Work::Upscaling, &list))) { return hr; }
        for (auto* resource : {color_.texture12.Get(), motion_.texture12.Get(), depth_.texture12.Get()}) {
            Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Transition(list, output_.texture12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        xess_d3d12_execute_params_t execute{};
        execute.pColorTexture = color_.texture12.Get(); execute.pVelocityTexture = motion_.texture12.Get();
        execute.pDepthTexture = depth_.texture12.Get(); execute.pOutputTexture = output_.texture12.Get();
        execute.jitterOffsetX = jitterX; execute.jitterOffsetY = jitterY; execute.exposureScale = 1;
        execute.resetHistory = reset || frames_ == 0; execute.inputWidth = input_.width; execute.inputHeight = input_.height;
        const auto execution = Result(xessD3D12Execute_(context_, list, &execute), "execute");
        for (auto* resource : {color_.texture12.Get(), motion_.texture12.Get(), depth_.texture12.Get()}) {
            Transition(list, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        Transition(list, output_.texture12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        // Submit even on an SDK error so recorded work has a retirement fence.
        if (FAILED(hr = transport_->Submit(SourceDLSSG::Work::Upscaling)) ||
            FAILED(hr = transport_->WaitD3D11(SourceDLSSG::Work::Upscaling))) { return hr; }
        if (FAILED(execution)) { return execution; }
        if (sharpness > 0 && FAILED(hr = Sharpen(context, sharpness))) { return hr; }
        context->CopyResource(destination, sharpness > 0 ? sharpened_.Get() : output_.texture11.Get());
        ++frames_; return S_OK;
    }
    HRESULT XeSSUpscaler::Sharpen(ID3D11DeviceContext* context, float strength)
    {
        auto hr = sharpenParameters_.Update(device_.Get(), context, strength);
        if (FAILED(hr)) { return hr; }
        if (!sharpenShader_) {
            Microsoft::WRL::ComPtr<ID3DBlob> code;
            // Intel/ is beside RCAS.hlsl in the installed pipeline directory.
            const auto path = directory_.parent_path() / L"RCAS.hlsl";
            if (FAILED(hr = D3DCompileFromFile(path.c_str(), nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, nullptr)) ||
                FAILED(hr = device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &sharpenShader_))) { return hr; }
        }
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> previous;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previousSRV;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> previousUAV;
        std::array<ID3D11ClassInstance*, 256> classes{}; UINT count = static_cast<UINT>(classes.size());
        context->CSGetShader(&previous, classes.data(), &count);
        context->CSGetShaderResources(0, 1, &previousSRV); context->CSGetUnorderedAccessViews(0, 1, &previousUAV);
        {
            auto parameters = sharpenParameters_.Bind(context);
            context->CSSetShader(sharpenShader_.Get(), nullptr, 0);
            context->CSSetShaderResources(0, 1, outputSRV_.GetAddressOf());
            context->CSSetUnorderedAccessViews(0, 1, sharpenUAV_.GetAddressOf(), nullptr);
            context->Dispatch((outputSize_.width + 7) / 8, (outputSize_.height + 7) / 8, 1);
            ID3D11ShaderResourceView* noSRV{}; ID3D11UnorderedAccessView* noUAV{};
            context->CSSetShaderResources(0, 1, &noSRV); context->CSSetUnorderedAccessViews(0, 1, &noUAV, nullptr);
        }
        context->CSSetShader(previous.Get(), classes.data(), count);
        context->CSSetShaderResources(0, 1, previousSRV.GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 1, previousUAV.GetAddressOf(), nullptr);
        for (UINT i = 0; i < count; ++i) { if (classes[i]) { classes[i]->Release(); } }
        return S_OK;
    }
    HRESULT XeSSUpscaler::ReleaseAfterRetirement()
    {
        // Caller owns the retirement boundary. On failure it retains this owner.
        if (context_) {
            const auto hr = Result(xessDestroyContext_(context_), "destroy context");
            if (FAILED(hr)) { return hr; }
            context_ = nullptr;
        }
        color_ = {}; motion_ = {}; depth_ = {}; output_ = {}; depthCopy_.ResetViews();
        sharpened_.Reset(); outputSRV_.Reset(); sharpenUAV_.Reset(); sharpenShader_.Reset();
        device_.Reset(); initialized_ = false; transport_ = nullptr; frames_ = 0; return S_OK;
    }
}
