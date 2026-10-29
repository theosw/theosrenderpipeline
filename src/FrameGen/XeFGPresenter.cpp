#include "XeFGPresenter.h"
#include "XeFGUnlock.h"
#include "PresenterReplacement.h"
#include "../PluginPaths.h"
#include <psapi.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>

namespace TheosRenderPipeline
{
    namespace
    {
        bool SameFile(const std::filesystem::path& a, const std::filesystem::path& b)
        {
            std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
            if (!left || !right) { return false; }
            const std::vector<char> x{std::istreambuf_iterator<char>(left), {}}, y{std::istreambuf_iterator<char>(right), {}};
            return !x.empty() && x == y;
        }
        // Modules with this base name in the process, whatever their folder.
        unsigned LoadedCount(const wchar_t* name)
        {
            // Grow until the snapshot is complete: a truncated one undercounts.
            std::vector<HMODULE> modules(1024);
            DWORD needed = 0;
            for (;;) {
                if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), DWORD(modules.size() * sizeof(HMODULE)), &needed)) { return 0; }
                if (needed <= modules.size() * sizeof(HMODULE)) { break; }
                modules.resize(needed / sizeof(HMODULE));
            }
            modules.resize(needed / sizeof(HMODULE));
            unsigned count = 0;
            for (const auto module : modules) {
                if (_wcsicmp(PluginPaths::ModulePath(module).filename().c_str(), name) == 0) { ++count; }
            }
            return count;
        }
    }
    HRESULT XeFGPresenter::FG(xefg_swapchain_result_t result, const char* operation)
    {
        if (result < 0) {
            status_ = std::string(operation) + " XeFG result=" + std::to_string(result);
            return result == XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY ? E_OUTOFMEMORY : E_FAIL;
        }
        if (result > 0) {
            ++snapshot_.warnings;
            if (log_ && (snapshot_.warnings <= 3 || result != lastWarning_ || operation != lastWarningOperation_ || snapshot_.warnings % 600 == 0)) {
                const auto message = std::string("warning operation=") + operation + " result=" + std::to_string(result) + " count=" + std::to_string(snapshot_.warnings);
                log_(message.c_str());
            }
            lastWarning_ = result; lastWarningOperation_ = operation;
        }
        return S_OK;
    }
    HRESULT XeFGPresenter::LL(xell_result_t result, const char* operation)
    {
        if (result == XELL_RESULT_SUCCESS) { return S_OK; }
        status_ = std::string(operation) + " XeLL result=" + std::to_string(result);
        return E_FAIL;
    }
    bool XeFGPresenter::SupportsFormat(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            format == DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    HMODULE XeFGPresenter::LoadedRuntime(const std::filesystem::path& directory) const
    {
        for (const auto& [folder, module] : runtimes_) {
            if (PluginPaths::EqualPath(folder, directory)) { return module; }
        }
        return nullptr;
    }
    HRESULT XeFGPresenter::Load(const std::filesystem::path& requested)
    {
        if (!requested.is_absolute()) { return E_INVALIDARG; }
        const auto directory = PluginPaths::Normalize(requested);
        if (LoadedRuntime(directory)) { return S_OK; }
        constexpr DWORD search = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
        if (!llModule_) {
            for (const auto* name : {L"libxell.dll", L"libxess_fg.dll"}) {
                if (GetModuleHandleW(name)) { status_ = "another owner already loaded an Intel runtime"; return E_UNEXPECTED; }
            }
            llModule_ = LoadLibraryExW((directory / L"libxell.dll").c_str(), nullptr, search);
            if (!llModule_) { status_ = "load libxell.dll"; return HRESULT_FROM_WIN32(GetLastError()); }
        } else if (!SameFile(PluginPaths::ModulePath(llModule_), directory / L"libxell.dll")) {
            // A second libxess_fg binds its import to the XeLL already loaded.
            status_ = "Intel SDK copies need an identical libxell.dll"; return E_UNEXPECTED;
        }
        const auto path = directory / L"libxess_fg.dll";
        const auto module = LoadLibraryExW(path.c_str(), nullptr, search);
        if (!module) { status_ = "load libxess_fg.dll"; return HRESULT_FROM_WIN32(GetLastError()); }
        if (!PluginPaths::EqualPath(PluginPaths::ModulePath(module), path) ||
            std::any_of(runtimes_.begin(), runtimes_.end(), [&](const auto& entry) { return entry.second == module; }) ||
            LoadedCount(L"libxell.dll") != 1) {
            status_ = "Intel SDK copy did not load beside the first"; return E_UNEXPECTED;
        }
        // Validate before accepting it: Activate runs only after the current
        // presenter has retired, when a failure can no longer keep it.
        for (const auto* name : {"xefgSwapChainGetVersion", "xefgSwapChainSetLoggingCallback", "xefgSwapChainD3D12CreateContext",
                "xefgSwapChainSetLatencyReduction", "xefgSwapChainGetProperties", "xefgSwapChainD3D12InitFromSwapChainDesc",
                "xefgSwapChainD3D12GetSwapChainPtr", "xefgSwapChainD3D12TagFrameResource", "xefgSwapChainTagFrameConstants",
                "xefgSwapChainSetPresentId", "xefgSwapChainSetEnabled", "xefgSwapChainSetNumInterpolatedFrames",
                "xefgSwapChainEnableDebugFeature", "xefgSwapChainGetLastPresentStatus", "xefgSwapChainSetUiCompositionState",
                "xefgSwapChainDestroy"}) {
            if (!GetProcAddress(module, name)) { status_ = std::string("missing ") + name; return E_NOINTERFACE; }
        }
        xefg_swapchain_version_t version{};
        const auto getVersion = reinterpret_cast<decltype(&::xefgSwapChainGetVersion)>(GetProcAddress(module, "xefgSwapChainGetVersion"));
        if (getVersion(&version) < 0 || version.major != 1 || version.minor != 3) {
            status_ = "candidate requires XeFG 1.3"; return E_NOINTERFACE;
        }
        runtimes_.emplace_back(directory, module);
        return S_OK;
    }
    HRESULT XeFGPresenter::Activate(const std::filesystem::path& directory)
    {
        const auto module = LoadedRuntime(directory);
        if (!module) { status_ = "Intel SDK copy not loaded"; return E_UNEXPECTED; }
        if (apiReady_ && module == fgModule_) { return S_OK; }
        if (initialized_ || fg_ || ll_) { status_ = "the Intel presenter must retire before switching SDK copies"; return E_UNEXPECTED; }
        apiReady_ = false; fgModule_ = module; directory_ = directory;
#define RESOLVE(module, name) name##_ = reinterpret_cast<decltype(name##_)>(GetProcAddress(module, #name)); \
        if (!name##_) { status_ = "missing " #name; return E_NOINTERFACE; }
        RESOLVE(fgModule_, xefgSwapChainGetVersion)
        RESOLVE(fgModule_, xefgSwapChainSetLoggingCallback)
        RESOLVE(fgModule_, xefgSwapChainD3D12CreateContext)
        RESOLVE(fgModule_, xefgSwapChainSetLatencyReduction)
        RESOLVE(fgModule_, xefgSwapChainGetProperties)
        RESOLVE(fgModule_, xefgSwapChainD3D12InitFromSwapChainDesc)
        RESOLVE(fgModule_, xefgSwapChainD3D12GetSwapChainPtr)
        RESOLVE(fgModule_, xefgSwapChainD3D12TagFrameResource)
        RESOLVE(fgModule_, xefgSwapChainTagFrameConstants)
        RESOLVE(fgModule_, xefgSwapChainSetPresentId)
        RESOLVE(fgModule_, xefgSwapChainSetEnabled)
        RESOLVE(fgModule_, xefgSwapChainSetNumInterpolatedFrames)
        RESOLVE(fgModule_, xefgSwapChainEnableDebugFeature)
        RESOLVE(fgModule_, xefgSwapChainGetLastPresentStatus)
        RESOLVE(fgModule_, xefgSwapChainSetUiCompositionState)
        RESOLVE(fgModule_, xefgSwapChainDestroy)
        RESOLVE(llModule_, xellGetVersion)
        RESOLVE(llModule_, xellD3D12CreateContext)
        RESOLVE(llModule_, xellSetSleepMode)
        RESOLVE(llModule_, xellSleep)
        RESOLVE(llModule_, xellAddMarkerData)
        RESOLVE(llModule_, xellDestroyContext)
#undef RESOLVE
        xefgSwapChainSetSceneChangeThreshold_ = reinterpret_cast<decltype(xefgSwapChainSetSceneChangeThreshold_)>(
            GetProcAddress(fgModule_, "xefgSwapChainSetSceneChangeThreshold"));
        xefg_swapchain_version_t fgVersion{}; xell_version_t llVersion{};
        auto hr = FG(xefgSwapChainGetVersion_(&fgVersion), "version");
        if (FAILED(hr)) { return hr; }
        if (FAILED(hr = LL(xellGetVersion_(&llVersion), "version"))) { return hr; }
        if (fgVersion.major != 1 || fgVersion.minor != 3 || llVersion.major != 1 || llVersion.minor != 3) {
            status_ = "candidate requires XeFG 1.3 and XeLL 1.3"; return E_NOINTERFACE;
        }
        apiReady_ = true;
        return S_OK;
    }
    HRESULT XeFGPresenter::Probe(ID3D12Device* device, const std::filesystem::path& directory, bool tensorRequested)
    {
        probedTensorRequest_ = snapshot_.tensorRequested = tensorRequested;
        // The helper is selected once, before the tensor SDK copy first loads.
        if (tensorRequested && !tensorChoiceMade_) {
            tensor_.Select(device, directory, true);
            tensorChoiceMade_ = true;
            if (log_) log_(tensor_.Status().c_str());
        }
        wantTensor_ = tensorRequested && tensor_.Selected();
        wanted_ = PluginPaths::Normalize(wantTensor_ ? tensor_.Directory() : directory);
        auto hr = Load(wanted_);
        if (FAILED(hr) && wantTensor_) {
            // A tensor copy that cannot load beside Intel's keeps normal XeFG.
            if (log_) log_(("Tensor model unavailable: " + status_ + "; normal XeFG retained").c_str());
            wantTensor_ = false; wanted_ = PluginPaths::Normalize(directory);
            hr = Load(wanted_);
        }
        if (FAILED(hr) || !device) { return FAILED(hr) ? hr : E_POINTER; }
        // A live presenter keeps its copy until it retires; Create switches then.
        const bool live = initialized_ || fg_ || ll_;
        if (!live) {
            if (FAILED(hr = Activate(wanted_))) { return hr; }
            if (wantTensor_) {
                hr = tensor_.Start(device, fgModule_, llModule_);
                if (log_) log_(tensor_.Status().c_str());
                if (FAILED(hr) && tensor_.Unsafe()) { status_ = tensor_.Status(); return E_UNEXPECTED; }
                // A safely refused model leaves this exact 1.3.4 SDK at official x2.
                // Dispatch-only refusal keeps the ported model's HLSL fallback.
            }
        }
        // Admission does not change within a session; skip another SDK context.
        if (device == admittedDevice_) { return S_OK; }
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_4};
        hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
        if (FAILED(hr) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_4) { status_ = "SM6.4 unavailable"; return DXGI_ERROR_UNSUPPORTED; }
        if (fg_) { return initialized_ ? S_OK : E_UNEXPECTED; }
        if (FAILED(hr = FG(xefgSwapChainD3D12CreateContext_(device, &fg_), "probe context"))) { return hr; }
        xefg_swapchain_properties_t props{};
        hr = FG(xefgSwapChainGetProperties_(fg_, &props), "capabilities");
        if (SUCCEEDED(hr) && props.maxSupportedInterpolations < 1) { hr = DXGI_ERROR_UNSUPPORTED; status_ = "x2 unavailable"; }
        const auto destroyed = FG(xefgSwapChainDestroy_(fg_), "destroy probe");
        if (FAILED(destroyed)) { return destroyed; } // Retain handle on failure.
        fg_ = nullptr;
        if (SUCCEEDED(hr)) { admittedDevice_ = device; }
        return hr;
    }
    HRESULT XeFGPresenter::Create(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGIFactory* factory,
        const DXGI_SWAP_CHAIN_DESC& desc, bool inverted, IDXGISwapChain** swapchain, std::uint32_t lastApplicationFrame, XeFGOptions options,
        ID3D12PipelineLibrary* pipelines, IDXGISwapChain* existingNative)
    {
        if (!swapchain) { return E_POINTER; }
        *swapchain = nullptr;
        if (!apiReady_ || initialized_ || fg_ || ll_ || !device || !queue || !factory || !SupportsFormat(desc.BufferDesc.Format)) { return E_INVALIDARG; }
        options = SanitizeXeFG(options);
        // The previous presenter has retired: bind the copy Probe chose.
        if (auto hr = Activate(wanted_); FAILED(hr)) { return hr; }
        usingTensor_ = wantTensor_;
        if (usingTensor_) {
            const auto started = tensor_.Start(device, fgModule_, llModule_);
            if (FAILED(started) && tensor_.Unsafe()) { status_ = tensor_.Status(); return E_UNEXPECTED; }
        }
        snapshot_.experimentalMFG = options.experimentalMFG || usingTensor_;
        snapshot_.unlockReady = false;
        snapshot_.maxGeneratedFrames = 1;
        snapshot_.generatedFrames = 1;
        unlockStatus_="Experimental MFG off; official x2";
        snapshot_.tensorModel = usingTensor_ && tensor_.ModelReady();
        snapshot_.tensorActive = usingTensor_ && (tensor_.Snapshot().flags & XeFGTensor::TensorReady) != 0;
        if (usingTensor_) {
            snapshot_.unlockReady = snapshot_.tensorModel;
            unlockStatus_ = tensor_.Status();
            if (log_) log_(unlockStatus_.c_str());
        } else if (options.experimentalMFG) {
            std::string reason;
            const auto patched = XeFGUnlock::EnsureInstalled(fgModule_, reason);
            unlockStatus_ = std::string(patched == XeFGUnlock::PatchResult::Applied ? "Experimental MFG ready: "
                                                                                    : "Experimental MFG refused: ") +
                reason;
            if (log_) log_(unlockStatus_.c_str());
            if (patched == XeFGUnlock::PatchResult::Unsafe) {
                status_ = "Unsafe XeFG patch state; provider stopped; restart required: " + reason;
                return E_UNEXPECTED;
            }
            snapshot_.unlockReady = patched == XeFGUnlock::PatchResult::Applied;
            if (!snapshot_.unlockReady) unlockStatus_ += "; official x2 retained";
        }
        XeFGUnlock::NewContextEpoch(); // Previous SDK context must already have retired.
        Trace("create latency context");
        auto hr = LL(xellD3D12CreateContext_(device, &ll_), "create latency context");
        if (FAILED(hr)) { return hr; }
        xell_sleep_params_t sleep{}; sleep.bLowLatencyMode = 1;
        if (FAILED(hr = LL(xellSetSleepMode_(ll_, &sleep), "enable latency before initialization")) ||
            FAILED(hr = FG(xefgSwapChainD3D12CreateContext_(device, &fg_), "create presenter")) ||
            FAILED(hr = FG(xefgSwapChainSetLatencyReduction_(fg_, ll_), "connect latency"))) { return hr; }
        xefg_swapchain_properties_t capabilities{};
        if (FAILED(hr = FG(xefgSwapChainGetProperties_(fg_, &capabilities), "initialized capabilities"))) return hr;
        if (capabilities.maxSupportedInterpolations < 1) {
            status_ = "XeFG x2 unavailable";
            return DXGI_ERROR_UNSUPPORTED;
        }
        snapshot_.maxGeneratedFrames =
            snapshot_.unlockReady ? std::min(XeFGMaxGeneratedFrames, capabilities.maxSupportedInterpolations) : 1u;
        snapshot_.generatedFrames = XeFGCount(options, snapshot_.maxGeneratedFrames);
        Trace("connect logging");
        if (FAILED(hr = FG(xefgSwapChainSetLoggingCallback_(fg_, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING,
            [](const char* message, xefg_swapchain_logging_level_t, void* context) {
                auto* owner = static_cast<XeFGPresenter*>(context);
                if (message) {
                    const auto native = XeFGNativeCreationResult(message);
                    if (native != E_FAIL) { owner->nativeCreationResult_.store(native); }
                    if (owner->log_) { owner->log_(message); }
                }
            }, this), "logging"))) { return hr; }
        DXGI_SWAP_CHAIN_DESC1 native{};
        native.Width = desc.BufferDesc.Width; native.Height = desc.BufferDesc.Height;
        native.Format = desc.BufferDesc.Format; native.SampleDesc = desc.SampleDesc;
        native.BufferUsage = desc.BufferUsage; native.BufferCount = 2;
        native.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        native.Flags = desc.Flags & ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        xefg_swapchain_d3d12_init_params_t init{};
        init.maxInterpolatedFrames = snapshot_.maxGeneratedFrames;
        // AUTO selects per frame from the tagged layers: HUD-less plus the UI
        // texture when the host has one, otherwise back-buffer extraction. Keep
        // AUTO for HDR10 too: the refinement mode Intel suggests for 2-bit UI alpha
        // requires a UI texture on every frame, and one frame without it fails
        // Present with E_FAIL ("UI mode requirements ... not met").
        init.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
        init.initFlags = inverted ? XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH : 0;
        // Shared across this session's presenters: a recreated one loads the
        // pipelines an earlier one stored instead of compiling them again.
        init.pPipelineLibrary = pipelines;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
        if (FAILED(hr = factory->QueryInterface(IID_PPV_ARGS(&factory2)))) { return hr; }
        nativeCreationResult_.store(S_OK);
        Trace(existingNative ? "initialize XeFG proxy with retained native HWND owner" : "initialize native swapchain from descriptor");
        factoryCapture_.Attach(new PresenterFactory(factory2.Get(), existingNative));
        hr = FG(xefgSwapChainD3D12InitFromSwapChainDesc_(fg_, desc.OutputWindow, &native,
            nullptr, queue, factoryCapture_.Get(), &init), "initialize swapchain");
        if (FAILED(hr)) {
            const auto nativeResult = nativeCreationResult_.load();
            return FAILED(nativeResult) ? nativeResult : hr;
        }
        if (FAILED(hr = FG(xefgSwapChainD3D12GetSwapChainPtr_(fg_, __uuidof(IDXGISwapChain),
                reinterpret_cast<void**>(swapchain)), "get proxy"))) { return hr; }
        Trace("configure proxy");
        initialized_ = true; snapshot_.invertedDepth = inverted; snapshot_.enabled = false;
        snapshot_.frameLimitUs = 0;
        snapshot_.framesPresented = 0; snapshot_.prepared = false; ++snapshot_.epoch;
        needsReset_ = true;
        // A new context starts with Intel's debug features off and no frame history.
        debugOnlyGenerated_ = debugTagGenerated_ = false;
        snapshot_.onlyGenerated = snapshot_.tagGenerated = false;
        sceneChangeThreshold_.reset(); snapshot_.sceneChangeThreshold = -1;
        lastFrameBegin_ = {}; frameIntervalMs_ = 0; snapshot_.frameTimeMs = 0;
        snapshot_.frameId = (std::max)(snapshot_.frameId, lastApplicationFrame);
        if (FAILED(hr = FG(xefgSwapChainSetNumInterpolatedFrames_(fg_, snapshot_.generatedFrames), "select multiplier")) ||
            FAILED(hr = FG(xefgSwapChainSetEnabled_(fg_, 0), "initial passthrough"))) { return hr; }
        pacingRegistered_ = false;
        if (usingTensor_) {
            const auto paced = tensor_.RegisterPacing(fg_, device, log_);
            if (log_) log_(tensor_.PacingStatus().c_str());
            if (FAILED(paced) && tensor_.Unsafe()) { status_ = tensor_.PacingStatus(); return paced; }
            pacingRegistered_ = paced == S_OK;
        }
        status_ = "XeFG x"+std::to_string(snapshot_.generatedFrames+1)+" ready; waiting for world inputs";
        return BeginFrame();
    }
    HRESULT XeFGPresenter::BeginFrame()
    {
        if (!initialized_ || frameBegun_ || snapshot_.frameId == (std::numeric_limits<std::uint32_t>::max)()) { return E_UNEXPECTED; }
        ++snapshot_.frameId; snapshot_.prepared = false;
        // Start-to-start application interval, including XeLL's sleep: the
        // inverse of the frame rate Intel's frameRenderTime describes.
        const auto now = std::chrono::steady_clock::now();
        frameIntervalMs_ = lastFrameBegin_ == std::chrono::steady_clock::time_point{} ? 0.0f :
            std::chrono::duration<float, std::milli>(now - lastFrameBegin_).count();
        lastFrameBegin_ = now;
        auto hr = LL(xellSleep_(ll_, snapshot_.frameId), "sleep");
        snapshot_.sleepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - now).count();
        if (FAILED(hr) || FAILED(hr = LL(xellAddMarkerData_(ll_, snapshot_.frameId, XELL_SIMULATION_START), "simulation start"))) { return hr; }
        frameBegun_ = true; return S_OK;
    }
    HRESULT XeFGPresenter::SetDebugView(bool onlyGenerated, bool tagGenerated)
    {
        if (!initialized_) { return S_OK; }
        HRESULT hr = S_OK;
        if (onlyGenerated != debugOnlyGenerated_) {
            if (FAILED(hr = FG(xefgSwapChainEnableDebugFeature_(fg_, XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION,
                onlyGenerated, nullptr), "debug view: only generated frames"))) { return hr; }
            debugOnlyGenerated_ = snapshot_.onlyGenerated = onlyGenerated;
        }
        if (tagGenerated != debugTagGenerated_) {
            if (FAILED(hr = FG(xefgSwapChainEnableDebugFeature_(fg_, XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES,
                tagGenerated, nullptr), "debug view: tag generated frames"))) { return hr; }
            debugTagGenerated_ = snapshot_.tagGenerated = tagGenerated;
        }
        return hr;
    }
    void XeFGPresenter::SetSceneChangeThreshold(float threshold)
    {
        if (!initialized_ || sceneChangeThreshold_ == threshold) { return; }
        // Recorded before the call so a refused value is reported once, not every frame.
        sceneChangeThreshold_ = threshold;
        if (!xefgSwapChainSetSceneChangeThreshold_) {
            if (log_) { log_("scene change threshold unavailable in this runtime; Intel's default retained"); }
            return;
        }
        const auto result = xefgSwapChainSetSceneChangeThreshold_(fg_, threshold);
        if (result < 0) {
            if (log_) {
                const auto text = "scene change threshold " + std::to_string(threshold) + " refused result=" + std::to_string(result);
                log_(text.c_str());
            }
            return;
        }
        (void)FG(result, "scene change threshold");
        snapshot_.sceneChangeThreshold = threshold;
        if (log_) {
            const auto text = "scene change threshold=" + std::to_string(threshold);
            log_(text.c_str());
        }
    }
    HRESULT XeFGPresenter::Prepare(const sl::Constants& c, bool frameTime)
    {
        if (!frameBegun_ || snapshot_.prepared) { return E_UNEXPECTED; }
        // CameraHistory already validates these bases and rebuilds an absolute
        // view from the camera position. Reproduce that same row-major view;
        // do not advance a second camera history for the other provider.
        const float direction = c.cameraViewToClip[2].w > 0 ? 1.0f : -1.0f;
        const sl::float3 z{c.cameraFwd.x * direction, c.cameraFwd.y * direction, c.cameraFwd.z * direction};
        const float v[]{c.cameraRight.x, c.cameraUp.x, z.x, 0, c.cameraRight.y, c.cameraUp.y, z.y, 0, c.cameraRight.z,
            c.cameraUp.z, z.z, 0,
            -(c.cameraPos.x * c.cameraRight.x + c.cameraPos.y * c.cameraRight.y + c.cameraPos.z * c.cameraRight.z),
            -(c.cameraPos.x * c.cameraUp.x + c.cameraPos.y * c.cameraUp.y + c.cameraPos.z * c.cameraUp.z),
            -(c.cameraPos.x * z.x + c.cameraPos.y * z.y + c.cameraPos.z * z.z), 1};
        constants_ = {};
        std::memcpy(constants_.viewMatrix, v, sizeof(v));
        std::memcpy(constants_.projectionMatrix, &c.cameraViewToClip, sizeof(constants_.projectionMatrix));
        constants_.jitterOffsetX = c.jitterOffset.x; constants_.jitterOffsetY = c.jitterOffset.y;
        constants_.resetHistory = needsReset_ || c.reset == sl::eTrue;
        // Intel: frame time in ms (the inverse of the frame rate), or 0 when
        // unavailable; non-Intel GPUs use it to sanity-check pacing. Skip the first
        // frame of a context and stalls such as loading or a paused game.
        constants_.frameRenderTime = frameTime && frameIntervalMs_ > 0 && frameIntervalMs_ <= 250 ? frameIntervalMs_ : 0;
        snapshot_.frameTimeMs = constants_.frameRenderTime;
        snapshot_.prepared = snapshot_.invertedDepth == (c.depthInverted == sl::eTrue);
        if (!snapshot_.prepared) { status_ = "depth convention changed; provider recreation required"; needsReset_ = true; }
        return S_OK;
    }
    HRESULT XeFGPresenter::BeforePresent(ID3D12GraphicsCommandList* list, ID3D12Resource* motion,
        ID3D12Resource* depth, ID3D12Resource* hudless, ID3D12Resource* ui, bool enabled, bool uiComposition, int outputFPSLimit, std::uint32_t generatedFrames)
    {
        if (!frameBegun_ || presentPending_) { return E_UNEXPECTED; }
        const bool generate = enabled && snapshot_.prepared && motion && depth && hudless;
        if (ui && hudless) {
            const auto u = ui->GetDesc(), h = hudless->GetDesc();
            if (u.Format != h.Format || u.Width != h.Width || u.Height != h.Height) { ui = nullptr; }
        }
        const bool tagUI = generate && uiComposition && ui;
        const auto count = XeFGCount({snapshot_.experimentalMFG, generatedFrames}, snapshot_.maxGeneratedFrames);
        if (count != snapshot_.generatedFrames) {
            const auto result = FG(xefgSwapChainSetNumInterpolatedFrames_(fg_, count), "live multiplier");
            if (FAILED(result)) return result;
            snapshot_.generatedFrames = count;
            ResetPacing();
        }
        auto hr = LL(xellAddMarkerData_(ll_, snapshot_.frameId, XELL_SIMULATION_END), "simulation end");
        if (FAILED(hr) || FAILED(hr = LL(xellAddMarkerData_(ll_, snapshot_.frameId, XELL_RENDERSUBMIT_START), "render start"))) { return hr; }
        xell_sleep_params_t sleep{}; sleep.bLowLatencyMode = 1;
        // The pinned XeLL runtime caps output deliveries. Multiplying by the
        // generation ratio would reduce a requested 60-output cap to 30/20/15.
        sleep.minimumIntervalUs = XeFGOutputInterval(outputFPSLimit);
        if (sleep.minimumIntervalUs != snapshot_.frameLimitUs &&
            FAILED(hr = LL(xellSetSleepMode_(ll_, &sleep), "output cap"))) { return hr; }
        if (FAILED(hr = FG(xefgSwapChainSetEnabled_(fg_, generate), "generation options"))) { return hr; }
        if (generate != snapshot_.enabled) ResetPacing();
        snapshot_.frameLimitUs = sleep.minimumIntervalUs;
        if (generate) {
            if (!list) { return E_POINTER; }
            if (FAILED(hr = FG(xefgSwapChainSetUiCompositionState_(fg_, uiComposition ?
                XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED : XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED), "UI composition"))) { return hr; }
            const std::pair<xefg_swapchain_resource_type_t, ID3D12Resource*> inputs[]{{XEFG_SWAPCHAIN_RES_DEPTH, depth},
                {XEFG_SWAPCHAIN_RES_MOTION_VECTOR, motion}, {XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, hudless}, {XEFG_SWAPCHAIN_RES_UI, ui}};
            for (const auto& pair : std::span(inputs, tagUI ? 4 : 3)) {
                const auto desc = pair.second->GetDesc();
                xefg_swapchain_d3d12_resource_data_t data{};
                data.type = pair.first; data.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
                data.resourceSize = {static_cast<std::uint32_t>(desc.Width), desc.Height};
                data.pResource = pair.second; data.incomingState = D3D12_RESOURCE_STATE_COMMON;
                if (FAILED(hr = FG(xefgSwapChainD3D12TagFrameResource_(fg_, list, snapshot_.frameId, &data), "copy frame input"))) { return hr; }
            }
            constants_.motionVectorScaleX = static_cast<float>(motion->GetDesc().Width);
            constants_.motionVectorScaleY = static_cast<float>(motion->GetDesc().Height);
            if (FAILED(hr = FG(xefgSwapChainTagFrameConstants_(fg_, snapshot_.frameId, &constants_), "frame constants"))) { return hr; }
            needsReset_ = false;
        } else { needsReset_ = true; }
        snapshot_.uiTexture = tagUI;
        if (tagUI) { ++snapshot_.uiTexturePresents; }
        snapshot_.enabled = generate; return S_OK;
    }
    HRESULT XeFGPresenter::FinalizePresent()
    {
        if (!frameBegun_ || presentPending_) { return E_UNEXPECTED; }
        auto hr = LL(xellAddMarkerData_(ll_, snapshot_.frameId, XELL_RENDERSUBMIT_END), "render end");
        if (FAILED(hr) ||
            FAILED(hr = FG(xefgSwapChainSetPresentId_(fg_, snapshot_.frameId), "present identity")) ||
            FAILED(hr = LL(xellAddMarkerData_(ll_, snapshot_.frameId, XELL_PRESENT_START), "present start"))) { return hr; }
        presentPending_ = true; return S_OK;
    }
    HRESULT XeFGPresenter::AfterPresent(HRESULT result)
    {
        if (FAILED(result)) { return result; }
        if (!presentPending_) { return E_UNEXPECTED; }
        auto hr = LL(xellAddMarkerData_(ll_, snapshot_.frameId, XELL_PRESENT_END), "present end");
        if (FAILED(hr)) { return hr; }
        frameBegun_ = presentPending_ = false;
        xefg_swapchain_present_status_t status{};
        if (FAILED(hr = FG(xefgSwapChainGetLastPresentStatus_(fg_, &status), "present status"))) { return hr; }
        if (FAILED(hr = FG(status.frameGenResult, "interpolation result"))) { return hr; }
        snapshot_.enabled = status.isFrameGenEnabled != 0;
        snapshot_.framesPresented = status.framesPresented;
        snapshot_.interpolationResult = status.frameGenResult;
        ++snapshot_.presents; snapshot_.totalOutputs += status.framesPresented;
        if (status.framesPresented > 1) { ++snapshot_.generatedPresents; }
        status_ = snapshot_.enabled ? "XeFG x"+std::to_string(snapshot_.generatedFrames+1)+" active" : "XeFG passthrough";
        if (usingTensor_) {
            const auto state = tensor_.Snapshot();
            snapshot_.tensorLaunches = state.launches; snapshot_.tensorFallbacks = state.fallbacks;
            snapshot_.pacing = tensor_.PacingStats();
            if (snapshot_.pacing.flags & XeFGPacing::Unsafe) {
                status_ = "XeFG driver metering state uncertain; provider stopped; restart required";
                return E_UNEXPECTED;
            }
        }
        if (log_ && (snapshot_.presents <= 3 || snapshot_.presents % 600 == 0)) {
            const auto pacing = XeFGUnlock::Snapshot();
            const auto text="present frame="+std::to_string(snapshot_.frameId)+" outputs="+std::to_string(snapshot_.framesPresented)+
                " requestedMultiplier="+std::to_string(snapshot_.generatedFrames+1)+" capacity="+std::to_string(snapshot_.maxGeneratedFrames+1)+
                " intervalUs="+std::to_string(snapshot_.frameLimitUs)+" processPacingCounters: presents="+std::to_string(pacing.presents)+
                " refused="+std::to_string(pacing.refused)+" wallWaits="+std::to_string(pacing.wallWaits)+
                " tensor="+std::to_string(snapshot_.tensorActive)+" tensorLaunches="+std::to_string(snapshot_.tensorLaunches)+
                " tensorFallbacks="+std::to_string(snapshot_.tensorFallbacks)+
                " pacingFlags="+std::to_string(snapshot_.pacing.flags)+" flipCalls="+std::to_string(snapshot_.pacing.flipCalls)+
                " meteredCalls="+std::to_string(snapshot_.pacing.meteredCalls)+" driverFailures="+std::to_string(snapshot_.pacing.driverFailures)+
                " pacedDeadlines="+std::to_string(snapshot_.pacing.deadlines);
            log_(text.c_str());
        }
        return S_OK;
    }
    HRESULT XeFGPresenter::Disable()
    {
        if (!initialized_) { return S_OK; }
        auto hr = FG(xefgSwapChainSetEnabled_(fg_, 0), "disable before retirement");
        if (SUCCEEDED(hr)) { snapshot_.enabled = false; ResetPacing(); }
        return hr;
    }
    HRESULT XeFGPresenter::Destroy()
    {
        if (factoryCapture_) { factoryCapture_->ReleaseNative(); }
        if (fg_) {
            const auto hr = FG(xefgSwapChainDestroy_(fg_), "destroy presenter and internal work");
            if (FAILED(hr)) { return hr; }
            if (pacingRegistered_) { retiredPacingContext_ = fg_; }
            pacingRegistered_ = false;
            fg_ = nullptr; initialized_ = false; frameBegun_ = presentPending_ = false;
        }
        if (retiredPacingContext_) {
            const auto hr = tensor_.UnregisterPacing(retiredPacingContext_);
            if (FAILED(hr)) { status_ = tensor_.PacingStatus(); return hr; }
            retiredPacingContext_ = nullptr;
        }
        if (ll_) {
            const auto hr = LL(xellDestroyContext_(ll_), "destroy latency context");
            if (FAILED(hr)) { return hr; }
            ll_ = nullptr;
        }
        const auto retired = tensor_.Retire();
        if (FAILED(retired)) { status_ = tensor_.Status(); return retired; }
        snapshot_.tensorModel = snapshot_.tensorActive = false;
        snapshot_.pacing = tensor_.PacingStats();
        snapshot_.prepared = snapshot_.enabled = false; needsReset_ = true;
        factoryCapture_.Reset();
        return S_OK;
    }
}
