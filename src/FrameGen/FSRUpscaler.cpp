#include "FSRUpscaler.h"
#include "D3D12Helpers.h"
#include "../PluginPaths.h"
#if defined(TRP_FSR4_FORCE_UNIVERSAL)
#include "FSR4Compatibility.h"
#endif
#include <dxgi1_4.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace TheosRenderPipeline
{
    HRESULT FSRUpscaler::Result(ffxReturnCode_t result, const char* operation)
    {
        if (result == FFX_API_RETURN_OK) { return S_OK; }
        status_ = std::string(operation) + " FSR result=" + std::to_string(result);
        return result == FFX_API_RETURN_ERROR_MEMORY ? E_OUTOFMEMORY : E_FAIL;
    }

    namespace
    {
        UINT AdapterVendor(ID3D12Device* device)
        {
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory; Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            DXGI_ADAPTER_DESC1 desc{};
            if (!device || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
                FAILED(adapter->GetDesc1(&desc))) { return 0; }
            return desc.VendorId;
        }
    }

    bool FSRUpscaler::FSR4NeedsForce(ID3D12Device* device)
    {
        if (!device) { return false; }
        // Cached per adapter LUID: the menu and live-settings paths ask every frame.
        const auto luid = D3D12Helpers::LuidToUInt64(device->GetAdapterLuid());
        static std::uint64_t cachedLuid{};
        static bool hasCached{}, needsForce{};
        if (!hasCached || luid != cachedLuid) {
            cachedLuid = luid;
            hasCached = true;
            needsForce = AdapterVendor(device) != 0x1002;
        }
        return needsForce;
    }

    const char* FSRUpscaler::FSR4Unavailable(ID3D12Device* device)
    {
        if (!device) { return "FSR 4 needs a D3D12 device"; }
        if (!FSR4NeedsForce(device)) { return nullptr; } // Radeon: AMD's runtime decides.
#if !defined(TRP_FSR4_FORCE_UNIVERSAL)
        return "FSR 4 on this GPU is Universal only";
#else
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
            model.HighestShaderModel < D3D_SHADER_MODEL_6_6) { return "FSR 4 needs Shader Model 6.6"; }
        D3D12_FEATURE_DATA_D3D12_OPTIONS4 options{};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options, sizeof(options))) ||
            !options.Native16BitShaderOpsSupported) { return "FSR 4 needs native 16-bit shader support"; }
        return nullptr;
#endif
    }

    HRESULT FSRUpscaler::Open(ID3D12Device* device, const std::filesystem::path& directory, bool forceFSR4)
    {
        if (!device || !directory.is_absolute()) { return E_INVALIDARG; }
        if (module_) {
            if (!PluginPaths::EqualPath(directory_, directory) || !create_ || !destroy_ || !dispatch_ || !query_) { return E_UNEXPECTED; }
            if (context_ && device12_.Get() != device) { return E_UNEXPECTED; }
            // A retired context may be recreated on a new device in this process.
            const bool deviceChanged = device12_.Get() != device;
            device12_ = device;
            if (deviceChanged) { QueryVersions(); }
            return S_OK;
        }
        directory_ = PluginPaths::Normalize(directory);
        for (const auto* name : {L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll", L"amd_fidelityfx_dx12.dll"}) {
            if (GetModuleHandleW(name)) { status_ = "another owner loaded " + PluginPaths::ToUtf8(name); return E_UNEXPECTED; }
        }
#if defined(TRP_FSR4_FORCE_UNIVERSAL)
        if (forceFSR4) {
            // Exact-hash runtime, patched in memory before AMD's first query.
            force_ = new FSR4Compatibility::Session();
            forced_ = force_->LoadVerified(directory_) && force_->ApplyBeforeFirstQuery(device, device->GetAdapterLuid(), true);
            forceAudit_ = force_->Audit();
            if (!forced_) { forceReason_ = force_->Reason(); }
            if (force_->Status() == FSR4Compatibility::Session::State::Poisoned) {
                status_ = "FSR 4 force left AMD's runtime in an uncertain state: " + forceReason_; return E_FAIL;
            }
            module_ = force_->Loader(); // only adopt a verified session's loader
        }
#else
        if (forceFSR4) { forceReason_ = "FSR 4 on this GPU is Universal only"; }
#endif
        // The SDK 2.4 loader finds amd_fidelityfx_upscaler_dx12.dll beside itself.
        if (!module_) {
            module_ = LoadLibraryExW((directory_ / L"amd_fidelityfx_loader_dx12.dll").c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
        if (!module_) { status_ = "AMD/amd_fidelityfx_loader_dx12.dll missing or not loadable; reinstall Theo's Render Pipeline"; return HRESULT_FROM_WIN32(GetLastError()); }
        device12_ = device;
        create_ = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module_, "ffxCreateContext"));
        destroy_ = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module_, "ffxDestroyContext"));
        dispatch_ = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module_, "ffxDispatch"));
        query_ = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module_, "ffxQuery"));
        if (!create_ || !destroy_ || !dispatch_ || !query_) { status_ = "missing FidelityFX API exports"; return E_NOINTERFACE; }
        QueryVersions();
        status_ = "FSR runtime loaded"; return S_OK;
    }

    void FSRUpscaler::QueryVersions()
    {
        versionIds_.clear(); versionNames_.clear();
        ffxQueryDescGetVersions query{}; query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; query.device = device12_.Get();
        std::uint64_t count = 0; query.outputCount = &count;
        if (query_(nullptr, &query.header) != FFX_API_RETURN_OK || !count || count > 64) { return; }
        std::vector<std::uint64_t> ids(count); std::vector<const char*> names(count);
        query.versionIds = ids.data(); query.versionNames = names.data();
        if (query_(nullptr, &query.header) != FFX_API_RETURN_OK) { return; }
        // AMD may overwrite name storage on later queries; keep copies.
        for (std::uint64_t i = 0; i < count; ++i) {
            versionIds_.push_back(ids[i]); versionNames_.emplace_back(names[i] ? names[i] : "");
        }
    }

    std::uint64_t FSRUpscaler::VersionId(const char* prefix) const
    {
        // AMD lists newest first. Prefer the SDK's own entry; driver entries end in " *".
        for (const bool driver : {false, true}) {
            for (std::size_t i = 0; i < versionNames_.size(); ++i) {
                if (versionNames_[i].starts_with(prefix) && versionNames_[i].ends_with("*") == driver) { return versionIds_[i]; }
            }
        }
        return 0;
    }

    bool FSRUpscaler::VerifyForcedModel() const { return ForcedModelCheck().empty(); }

    std::string FSRUpscaler::ForcedModelCheck() const
    {
#if defined(TRP_FSR4_FORCE_UNIVERSAL)
        HMODULE upscaler = force_ ? force_->Upscaler() : nullptr;
        if (!upscaler) { upscaler = GetModuleHandleW(L"amd_fidelityfx_upscaler_dx12.dll"); }
        return FSR4Compatibility::VerifyForcedModel(context_, upscaler);
#else
        return "Universal only";
#endif
    }

    std::string FSRUpscaler::RunningLabel() const
    {
        if (!initialized_) { return {}; }
        if (active_ == FSRVersion::FSR4 && forced_) {
            return verified_ ? "FSR " + version_ + " INT8 (forced)" : "FSR " + version_ + " forced; not verified running";
        }
        return "FSR " + version_;
    }

    bool FSRUpscaler::RuntimePresent()
    {
        std::error_code error;
        return std::filesystem::is_regular_file(PluginPaths::AMDDirectory() / L"amd_fidelityfx_loader_dx12.dll", error) &&
            std::filesystem::is_regular_file(PluginPaths::AMDDirectory() / L"amd_fidelityfx_upscaler_dx12.dll", error);
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
        // SDK 2.1+ requires the API version the caller was built against.
        apiVersion_ = {}; apiVersion_.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, &backend_.header};
        apiVersion_.version = FFX_UPSCALER_VERSION;
        ffxApiHeader* chain = &apiVersion_.header;
        // Every context pins its version (ids come from AMD's query, never hard-coded).
        override_ = {}; override_.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        active_ = requested_; fallback_.clear(); verified_ = false;
        override_.versionId = VersionId(active_ == FSRVersion::FSR4 ? "4." : "3.");
        if (!override_.versionId && active_ == FSRVersion::FSR4) {
            fallback_ = !forceReason_.empty() ? "FSR 4 refused: " + forceReason_
                : forced_ ? std::string("FSR 4 not offered after the force")
                : std::string("AMD doesn't run FSR 4 on this GPU");
            active_ = FSRVersion::FSR31; override_.versionId = VersionId("3.");
        }
        if (!override_.versionId) { status_ = "FSR 3.1 is not offered by this AMD runtime"; return E_NOINTERFACE; }
        override_.header.pNext = chain; chain = &override_.header;
        description_ = {}; description_.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, chain};
        // TRP's SDR world is already tone mapped by Skyrim/ENB. Exposure is 1;
        // native UI and TRP HDR output are composed after temporal reconstruction.
        description_.flags = invertedDepth ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0;
        if (format_ == DXGI_FORMAT_R8G8B8A8_UNORM) { description_.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE; }
        else { description_.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE; }
        description_.maxRenderSize = {input_.width, input_.height};
        description_.maxUpscaleSize = {outputSize_.width, outputSize_.height};
        auto hr = Result(create_(&context_, &description_.header, nullptr), "create context");
        if (FAILED(hr) && active_ == FSRVersion::FSR4) {
            // FSR 4 offered but would not start: run FSR 3.1 and say why.
            fallback_ = "FSR 4 failed to start (" + status_ + ")";
            context_ = nullptr; active_ = FSRVersion::FSR31; override_.versionId = VersionId("3.");
            if (!override_.versionId) { status_ = "FSR 3.1 is not offered by this AMD runtime"; return E_NOINTERFACE; }
            hr = Result(create_(&context_, &description_.header, nullptr), "create FSR 3.1 context after FSR 4 failed");
        }
        if (FAILED(hr)) { return hr; }
        invertedDepth_ = invertedDepth;
        ffxQueryGetProviderVersion version{}; version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        const auto queried = Result(query_(&context_, &version.header), "query provider version");
        if (FAILED(queried) || !version.versionName) {
            destroy_(&context_, nullptr);
            context_ = nullptr;
            return FAILED(queried) ? queried : E_UNEXPECTED;
        }
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
            FAILED(hr = CreateContext(invertedDepth))) {
            color_ = {}; motion_ = {}; depth_ = {}; output_ = {}; depthCopy_.ResetViews(); transport_ = nullptr;
            return hr;
        }
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
        // Both APIs own these resources in COMMON between frames. AMD asks for
        // inputs in NON_PIXEL_SHADER_RESOURCE (required by FSR 4) and a UAV output,
        // so transition explicitly and declare those exact states.
        ID3D12Resource* inputs[]{color_.texture12.Get(), depth_.texture12.Get(), motion_.texture12.Get()};
        auto transition = [&](D3D12_RESOURCE_STATES inputState, D3D12_RESOURCE_STATES outputState,
                              D3D12_RESOURCE_STATES inputAfter, D3D12_RESOURCE_STATES outputAfter) {
            std::array<D3D12_RESOURCE_BARRIER, 4> barriers{};
            for (std::size_t i = 0; i < 4; ++i) {
                auto& b = barriers[i]; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = i < 3 ? inputs[i] : output_.texture12.Get();
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = i < 3 ? inputState : outputState;
                b.Transition.StateAfter = i < 3 ? inputAfter : outputAfter;
            }
            list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        };
        transition(D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        execute.color = ffxApiGetResourceDX12(color_.texture12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        execute.depth = ffxApiGetResourceDX12(depth_.texture12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        execute.motionVectors = ffxApiGetResourceDX12(motion_.texture12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        execute.output = ffxApiGetResourceDX12(output_.texture12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
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
        transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
        // Fence even failed dispatches: partially recorded SDK work still owns memory.
        if (FAILED(hr = transport_->Submit(SourceDLSSG::Work::Upscaling)) ||
            FAILED(hr = transport_->WaitD3D11(SourceDLSSG::Work::Upscaling))) { return hr; }
        if (FAILED(execution)) { return execution; }
        context->CopyResource(destination, output_.texture11.Get());
        ++frames_;
        // The INT8 model is built lazily on the first dispatch; check it once it exists.
        if (forced_ && active_ == FSRVersion::FSR4 && !verified_ && frames_ >= 2) { verified_ = VerifyForcedModel(); }
        return S_OK;
    }

    HRESULT FSRUpscaler::ReleaseAfterRetirement()
    {
        if (context_) {
            const auto hr = Result(destroy_(&context_, nullptr), "destroy context");
            if (FAILED(hr)) { return hr; }
            context_ = nullptr;
        }
        color_ = {}; motion_ = {}; depth_ = {}; output_ = {}; depthCopy_.ResetAfterRetirement();
        description_ = {}; backend_ = {}; // Keep the opened device for same-device reinitialization.
        initialized_ = false; transport_ = nullptr; frames_ = 0; lastFrame_ = {}; return S_OK;
    }
}
