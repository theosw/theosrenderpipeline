#include "FrameGen/VulkanNRHost.h"
#include <PCH.h>
#include "OverlayUI.h"
#include "FrameGen/NeuralRuntimeSession.h"
#include "OverlayFrameView.h"
#include "RenderPipeline.h"
#include "CommunityShaderIntegration.h"
#include "ReShadeIntegration.h"
#include "WeatherAppearanceRuntime.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "FrameGen/DlssgLiteBlending.h"
#include "FrameGen/NeuralRuntimeFiles.h"
#include "FrameGen/NeuralRuntimeKernels.h"
#include "FrameGen/NeuralRenderingFeatureSession.h"
#include "FrameGen/XeFGUnlock.h"
#include "PluginPaths.h"
#include "NeuralObjectHooks.h"

#include "NvidiaAppProfile.h"
#include "SessionDegradation.h"
#include "StartupHints.h"
#include "FrameGen/NvidiaDriverVersion.h"
#include "FrameGen/D3D12Helpers.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace
{
    // The driver profile is read once, on a worker thread when the menu first
    // opens: it loads the whole profile store, and NVIDIA App changes need a
    // restart of the game anyway. Until it finishes the menu shows no override.
    TheosRenderPipeline::NvidiaApp::Overrides NvidiaAppOverrides()
    {
        struct SharedState
        {
            std::mutex lock;
            TheosRenderPipeline::NvidiaApp::Overrides result;
        };
        static const auto state = std::make_shared<SharedState>();
        static std::once_flag started;
        std::call_once(started, [shared = state] {
            std::thread([shared] {
                const auto values = TheosRenderPipeline::NvidiaApp::ReadProfile();
                const auto overrides = TheosRenderPipeline::NvidiaApp::Interpret(values);
                spdlog::info("[NvidiaApp] profile {}: upscaling {} ({}), frame generation {}, NR {} ({})",
                    overrides.read ? "read" : "unavailable", overrides.upscaling, overrides.upscalingModel,
                    overrides.frameGeneration, overrides.neural, overrides.neuralModel);
                std::scoped_lock guard(shared->lock);
                shared->result = overrides;
            }).detach();
        });
        std::scoped_lock guard(state->lock);
        return state->result;
    }
}

namespace
{
    std::string FileVersion(const std::filesystem::path& path)
    {
        DWORD ignored{};
        const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
        std::vector<std::byte> data(size);
        VS_FIXEDFILEINFO* info{};
        UINT infoSize{};
        if (!size || !GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
            !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) || !info || infoSize < sizeof(VS_FIXEDFILEINFO)) {
            return {};
        }
        return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
            HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    }
}

// Read when Copy details is pressed, not every frame: it walks the module list
// and reads file versions. Everything here is also in TheosRenderPipeline.log.
TheosRenderPipeline::Support::Summary OverlayUI::CaptureSupportSystem()
{
    namespace SD = TheosRenderPipeline::SourceDLSSG;
    TheosRenderPipeline::Support::Summary summary;
    summary.release = Plugin::RELEASE_VERSION;
    summary.edition = Plugin::EDITION;
    summary.source = Plugin::SOURCE_REVISION;
    summary.skyrim = REL::Module::get().version().string();
    const auto& backend = SD::Backend::Get();
    summary.gpu = backend.AdapterName();
    if (const auto driver = backend.AdapterDriver(); driver.QuadPart) {
        summary.driver = std::format("{}.{}.{}.{}", HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart));
        if (const auto nvidia = backend.NvidiaAdapter() ? SD::NvidiaDriverVersion(driver.LowPart) : 0u) {
            summary.driver = SD::DriverVersionText(nvidia) + " (" + summary.driver + ")";
        }
    }
    if (summary.gpu.empty() && NvidiaHost::GetSingleton()->SteppedAside()) { summary.gpu = "not read (TRP stepped aside before starting its renderer)"; }  // not translated: support text is English, like the log
    wchar_t profile[MAX_PATH]{};
    if (const auto length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH); length && length < MAX_PATH) {
        summary.userProfile = TheosRenderPipeline::PluginPaths::ToUtf8(std::wstring_view(profile, length));
    }
    // Vendor runtimes, other renderers' files and TRP itself, by file name.
    static constexpr std::wstring_view prefixes[]{L"theosrenderpipeline", L"trp", L"nvngx", L"_nvngx", L"sl.", L"nvpresent", L"libxess", L"libxell",
        L"amd_fidelityfx", L"amdhip64", L"dlss5nr", L"communityshaders", L"reshade", L"pdperfplugin", L"skyrimupscaler", L"d3d11.dll", L"dxgi.dll",
        L"d3d12core", L"enbseries", L"optiscaler"};
    wchar_t system[MAX_PATH]{};
    std::wstring systemFolder(system, GetSystemDirectoryW(system, MAX_PATH));
    if (!systemFolder.empty() && systemFolder.back() != L'\\' && systemFolder.back() != L'/') {
        systemFolder.push_back(L'\\');
    }
    for (const auto module : TheosRenderPipeline::PluginPaths::LoadedModules()) {
        const auto path = TheosRenderPipeline::PluginPaths::RawModulePath(module);
        auto name = path.filename().wstring();
        std::wstring lower = name;
        for (auto& character : lower) { character = static_cast<wchar_t>(towlower(character)); }
        bool listed = false;
        for (const auto prefix : prefixes) { listed |= lower.starts_with(prefix); }
        // Windows' own d3d11.dll and dxgi.dll say nothing; a copy elsewhere is ENB, ReShade or another wrapper.
        if (listed && (lower == L"d3d11.dll" || lower == L"dxgi.dll") && !systemFolder.empty() &&
            _wcsnicmp(path.c_str(), systemFolder.c_str(), systemFolder.size()) == 0) { listed = false; }
        if (listed) {
            summary.modules.push_back({TheosRenderPipeline::PluginPaths::ToUtf8(path.filename()),
                FileVersion(path), TheosRenderPipeline::PluginPaths::ToUtf8(path)});
        }
    }
    std::sort(summary.modules.begin(), summary.modules.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
    return summary;
}

// The only place the menu reads the renderer. Drawing uses the returned view.
OverlayUI::FrameView OverlayUI::CaptureFrameView()
{
    FrameView view;

    auto upscaler = RenderPipeline::GetSingleton();
    auto textureProvider = TextureProviderBridge::GetSingleton();
    auto videoMemory = VideoMemoryTelemetry::GetSingleton();
    if (!settingsDraft.valid)
    {
        CaptureSettingsDraft();
    }
    TextureProviderBridge::Settings observedTextureSettings{};
    view.textureProviderAvailable = textureProvider->Read(observedTextureSettings, &view.textureTelemetry);
    view.textureProviderStatus = textureProvider->Status();
    videoMemory->Update();
    view.memorySnapshot = videoMemory->GetSnapshot();
    view.memoryStatus = videoMemory->Status();
    if (view.textureProviderAvailable && !settingsDraft.textureProviderConnected)
    {
        settingsDraft.textureProviderConnected = true;
        settingsDraft.textureProviderSettings = observedTextureSettings;
    }

    view.communityShaders = TheosRenderPipeline::CommunityShaders::Active();
    auto* nvidiaHost = NvidiaHost::GetSingleton();
    view.nvidiaHostActive = nvidiaHost->ProxyActive();
    view.hostStartupConfigured = nvidiaHost->StartupConfigured();
    view.upscalerReady = nvidiaHost->UpscalerReady();
    view.dedicatedUITexture = nvidiaHost->DedicatedUITextureMode();
    view.hostFrameGenerationEnabled = nvidiaHost->FrameGenerationEnabled();
    view.evaluationCount = nvidiaHost->EvaluationCount();
    view.warmupPresentsRemaining = nvidiaHost->WarmupPresentsRemaining();
    view.renderWidth = nvidiaHost->RenderWidth();
    view.renderHeight = nvidiaHost->RenderHeight();
    view.outputWidth = nvidiaHost->OutputWidth();
    view.outputHeight = nvidiaHost->OutputHeight();
    view.xessActive = nvidiaHost->XeSSActive();
    view.nrFriendlyRenderSizeActive = nvidiaHost->NRFriendlyRenderSizeActive();
    view.fsrActive = nvidiaHost->FSRActive();
    // Runtime installation requires a restart. Do not scan the filesystem every frame.
    static const bool fsrRuntimePresent = TheosRenderPipeline::FSRUpscaler::RuntimePresent();
    view.fsrRuntimePresent = fsrRuntimePresent;
    {
        const auto& fsr = TheosRenderPipeline::SourceDLSSG::Backend::Get().FSR();
        view.fsrRunning = fsr.RunningLabel();
        view.fsrFallback = fsr.Ready() ? fsr.FallbackReason() : std::string();
        auto* device = TheosRenderPipeline::SourceDLSSG::Backend::Get().Device12();
        static std::uint64_t checkedLuid{}; static bool hasChecked{}; static const char* reason{}; static bool needsForce{};
        if (device) {
            const auto luid = TheosRenderPipeline::D3D12Helpers::LuidToUInt64(device->GetAdapterLuid());
            if (!hasChecked || luid != checkedLuid) {  // the adapter's capabilities do not change
                checkedLuid = luid; hasChecked = true;
                reason = TheosRenderPipeline::FSRUpscaler::FSR4Unavailable(device);
                needsForce = TheosRenderPipeline::FSRUpscaler::FSR4NeedsForce(device);
            }
        }
        view.fsr4Reason = device ? reason : TheosRenderPipeline::FSRUpscaler::FSR4Unavailable(nullptr);
        view.fsr4NeedsRestart = device && needsForce && !fsr.Forced();
        view.fsrWatermarkActive = GetEnvironmentVariableW(L"MLSR-WATERMARK", nullptr, 0) > 0;
    }
    const auto& upscalerSettings = nvidiaHost->SourceUpscalerSettings();
    view.upscaler = upscalerSettings.Effective();
    view.upscalerNeedsRestart = upscalerSettings.NeedsRestart();
    view.upscalerFailed = upscalerSettings.Failed();
    view.upscalerChangeQueued = upscalerSettings.NeedsLiveChange();
    view.upscalerLiveModeRejected = upscalerSettings.LiveModeRejected();
    view.upscalerStartupMode = upscalerSettings.Startup().mode;
    view.steppedAside = nvidiaHost->SteppedAside();

    view.renderSizeX = upscaler->mRenderSizeX;
    view.renderSizeY = upscaler->mRenderSizeY;
    view.displaySizeX = upscaler->mDisplaySizeX;
    view.displaySizeY = upscaler->mDisplaySizeY;
    view.nativeUI = upscaler->mNativeUI;
    view.reShadeBeforeUpscaling = upscaler->mReShadeBeforeUpscaling;
    view.reShadeStatus = TheosRenderPipeline::ReShadeIntegration::Get().Status();
    view.menuHotkey = upscaler->mToggleOverlayHotkey;

    auto& backend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    auto* frameGen = SourceFrameGeneration::GetSingleton();
    view.frameGenerationRequested = frameGen->RuntimeInterpolationRequested();
    view.refreshRate = frameGen->refreshRate;
    view.neuralRuntimePath = frameGen->settings.neuralRenderingRuntimePath;
#if !defined(TRP_NO_NEURAL_RENDERING)
    // Folder listings go through MO2's virtual file system; refresh every few seconds.
    using Clock = std::chrono::steady_clock;
    static std::vector<std::string> runtimes;
    static std::string listedFor;
    static Clock::time_point listed{};
    const auto& selectedRuntime = settingsDraft.sourceDLSSG.neuralRuntime;
    if (listedFor != selectedRuntime || Clock::now() - listed > std::chrono::seconds(3))
    {
        runtimes = TheosRenderPipeline::SourceDLSSG::ListNeuralRuntimes(TheosRenderPipeline::PluginPaths::Directory(),
                                                                        selectedRuntime);
        listedFor = selectedRuntime;
        listed = Clock::now();
    }
    view.neuralRuntimes = runtimes;
#endif

    view.sourceReady = backend.Ready();
#if !defined(TRP_NO_NEURAL_RENDERING)
    static TheosRenderPipeline::NeuralRendering::NeuralCapability admission;
    static Clock::time_point admittedAt{};
    static TheosRenderPipeline::SourceDLSSG::Preferences admittedPreferences;
    static std::vector<std::string> runtimeReasons, admittedRuntimes;
    static std::string hipReason, vulkanReason, vulkanPath;
    static bool checksPending{};
    // While GTX 10 runtime checks run in the background, look again soon after they finish.
    if (Clock::now() - admittedAt > (checksPending ? std::chrono::milliseconds(250) : std::chrono::seconds(3)) ||
        admittedRuntimes != view.neuralRuntimes ||
        admittedPreferences.neuralRuntime != settingsDraft.sourceDLSSG.neuralRuntime ||
        !TheosRenderPipeline::SameNeuralPreferences(admittedPreferences, settingsDraft.sourceDLSSG)) {
        auto options = backend.NeuralConfiguration();
        const auto& draft = settingsDraft.sourceDLSSG;
        options.runtimePath = TheosRenderPipeline::SourceDLSSG::ResolveNeuralRuntimeSetting(draft.neuralRuntime,
            TheosRenderPipeline::PluginPaths::Directory(), backend.NeuralComputeCapability(),
            TheosRenderPipeline::NeuralRendering::RuntimeKernelsSupported());
        options.backend = draft.neuralBackend;
        options.passes = draft.neuralPasses;
        options.beforeUpscaling = draft.neuralBeforeUpscaling;
        options.reconstruction = draft.neuralReconstruction;
        options.extraPasses = draft.neuralExtraPasses;
        options = TheosRenderPipeline::SourceDLSSG::NormalizeNeuralRoute(std::move(options), view.communityShaders);
        // Menu checks never hash on the render thread (that froze the first menu frame on GTX 10).
        admission = backend.CheckNeuralAdmission(options, options.beforeUpscaling ? view.renderWidth : view.outputWidth,
            options.beforeUpscaling ? view.renderHeight : view.outputHeight, false, true);
        checksPending = admission.assetsPending;
        runtimeReasons.clear();
        for (const auto& runtime : view.neuralRuntimes) {
            const auto path = TheosRenderPipeline::SourceDLSSG::ResolveNeuralRuntimeSetting(runtime,
                TheosRenderPipeline::PluginPaths::Directory(), backend.NeuralComputeCapability(),
                TheosRenderPipeline::NeuralRendering::RuntimeKernelsSupported());
            const auto verified = !admission.Pascal() ? std::optional<bool>(true) :
                admission.universal ? TheosRenderPipeline::NeuralRendering::PollPascalAssets(path) : std::optional<bool>(false);
            if (!verified) {
                checksPending = true;
                runtimeReasons.emplace_back(TheosRenderPipeline::NeuralRendering::kPascalAssetsChecking);
                continue;
            }
            const auto* reason = TheosRenderPipeline::NeuralRendering::NeuralRuntimeChoiceReason(admission.Pascal(), *verified);
            // TRP Fast replaces TRP's RTX 30/40 kernels; elsewhere it has nothing to run.
            const int capability = backend.NeuralComputeCapability();
            if (!reason && TheosRenderPipeline::SourceDLSSG::DescribeNeuralRuntime(runtime).detail ==
                    TheosRenderPipeline::SourceDLSSG::NeuralRuntimeDetail::TRPFastKernels) {
                reason = !TheosRenderPipeline::NeuralRendering::RuntimeKernelsSupported() ? TheosRenderPipeline::NeuralRendering::kFastPackEditionReason :
                    capability >= 0 && capability != 86 && capability != 89 ? TheosRenderPipeline::NeuralRendering::kFastPackGPUReason : nullptr;
            }
            runtimeReasons.emplace_back(reason ? reason : "");
        }
        const auto* hip = TheosRenderPipeline::SourceDLSSG::AmdNeuralPass::Availability(backend.AmdAdapter());
        hipReason = hip ? hip : "";
        const auto vk = TheosRenderPipeline::NeuralRendering::VulkanNRHost::Probe(backend.Device12(), false,
            draft.neuralBackend == TheosRenderPipeline::NeuralRendering::Backend::Vulkan);
        vulkanReason = vk.available ? "" : vk.reason;
        vulkanPath = vk.pathReason;
        checksPending |= vulkanReason == TheosRenderPipeline::NeuralRendering::kVulkanChecking;
        admittedRuntimes = view.neuralRuntimes;
        admittedPreferences = draft; admittedAt = Clock::now();
    }
    view.neuralRuntimeReasons = runtimeReasons;
    view.neuralHIPReason = hipReason;
    if (const auto* reason = TheosRenderPipeline::NeuralRendering::runtimeSession.Refusal(TheosRenderPipeline::NeuralRendering::Backend::HIP)) { view.neuralHIPReason = reason; }
    view.neuralVulkanReason = vulkanReason;
    if (const auto* reason = TheosRenderPipeline::NeuralRendering::runtimeSession.Refusal(TheosRenderPipeline::NeuralRendering::Backend::Vulkan)) { view.neuralVulkanReason = reason; }
    view.neuralVulkanPath = vulkanPath;
    if (settingsDraft.sourceDLSSG.neuralBackend == TheosRenderPipeline::NeuralRendering::Backend::HIP) {
        view.neuralRuntimePath = TheosRenderPipeline::PluginPaths::ToUtf8(TheosRenderPipeline::SourceDLSSG::AmdNeuralPass::RuntimePath());
    } else if (settingsDraft.sourceDLSSG.neuralBackend == TheosRenderPipeline::NeuralRendering::Backend::Vulkan) {
        view.neuralRuntimePath = TheosRenderPipeline::PluginPaths::ToUtf8(TheosRenderPipeline::NeuralRendering::VulkanNRHost::Path());
    }
    view.neuralPascal = admission.Pascal();
    view.neuralAdmissionReason = admission.Refusal() ? admission.Refusal() : "";
#endif
    view.nvidiaAdapter = backend.NvidiaAdapter();
    view.amdAdapter = backend.AmdAdapter();
    view.nvidiaFeatures = backend.NvidiaFeaturesAvailable();
    view.nvidiaFrameGeneration = backend.NvidiaFrameGenerationAvailable();
    // Smooth Motion is the driver's own component in this process; it needs no adapter
    // check, and TRP's backend never starts when TRP stepped aside for it.
    view.nvidiaApp.smoothMotion = TheosRenderPipeline::NvidiaApp::SmoothMotionLoaded() ? 1 : 0;
    if (view.nvidiaAdapter) {
        const auto app = NvidiaAppOverrides();
        view.nvidiaApp.profileRead = app.read;
        view.nvidiaApp.upscalingModelOverride = app.upscaling;
        view.nvidiaApp.upscalingModel = app.upscalingModel;
        // Streamline's own log also reports an NVIDIA App frame-generation override.
        view.nvidiaApp.frameGenerationOverride = app.frameGeneration || backend.FrameGenerationOverrideReported();
        view.nvidiaApp.neuralModelOverride = app.neural;
        view.nvidiaApp.neuralModel = app.neuralModel;
    }
    {
        // The facts behind the advice read the registry and module list: gather
        // them once per change of the record, not every menu frame.
        static std::uint32_t described = ~0u;
        static TheosRenderPipeline::StartupHints::Facts facts;
        auto& record = TheosRenderPipeline::Degradation::Session();
        view.degraded = record.Entries();
        if (const auto revision = record.Revision(); revision != described) {
            described = revision;
            std::string status;
            for (const auto& entry : view.degraded) {
                if (entry.reason == TheosRenderPipeline::Degradation::Reason::FrameGenerationStartup) { status = entry.detail; }
            }
            facts = view.degraded.empty() ? TheosRenderPipeline::StartupHints::Facts{} :
                TheosRenderPipeline::StartupHints::CurrentFacts(status);
        }
        view.degradedFacts = facts;
    }
    view.videoMemoryFull = backend.VideoMemoryFullRecently();
    view.nvidiaServicesReady = backend.NvidiaServicesReady();
    view.generationActive = backend.GenerationActive();
    view.effectiveMultiplier = backend.EffectiveMultiplier();
    // The startup transport has no presenter; the menu names the provider that was asked for.
    view.provider = backend.StartupTransport() ? backend.RequestedProvider() : backend.Provider();
    view.requestedProvider = backend.RequestedProvider();
    view.providerStatus = backend.ProviderStatus();
    view.nvidiaSwitchBlocked = backend.NvidiaSwitchBlocked();
    view.uiRecomposition = backend.UIRecompositionConfiguration();
    view.xefg = backend.XeFGState();
    view.xefgUnlockStatus = backend.XeFGUnlockStatus();
    view.xefgUnlockAvailable = TheosRenderPipeline::XeFGUnlock::Available();
    view.liteBlending = TheosRenderPipeline::SourceDLSSG::LiteBlending::State();
    const auto vsync = backend.VSyncState();
    view.vsyncObserved = vsync.observed;
    view.vsyncIncoming = vsync.submission.incomingSync;
    view.vsyncRequested = vsync.submission.sync;
    view.vsyncFailed = FAILED(vsync.result);
    view.vsyncRequiredOff = vsync.submission.autoRequiredOff;
    view.vsyncConfigured = backend.VSyncConfigurationMode();
    view.xefgTensorRequested = backend.XeFGTensorRequested();
    view.xefgTensorStatus = backend.XeFGTensorStatus();
    view.xefgPacingStatus = backend.XeFGPacingStatus();
    view.xefgFlipMetering = backend.XeFGFlipMetering();
    view.xefgEvenPacingInactive = backend.XeFGVSyncCadenceUnavailable();
    view.steadyRenderedFPS = backend.SteadyRenderedFPS();
    view.steadyWithXeFG = backend.SteadyWithXeFG();
    view.session = backend.Snapshot();
    view.mfg = backend.MFGState();
    view.neuralApplied = backend.NeuralConfiguration();
    view.neuralState = backend.NeuralState();
#if !defined(TRP_NO_NEURAL_RENDERING)
    view.appliedNeuralRuntimePath = TheosRenderPipeline::PluginPaths::ToUtf8(view.neuralApplied.runtimePath);
    if (view.neuralApplied.backend == settingsDraft.sourceDLSSG.neuralBackend) {
        view.appliedNeuralAdmissionReason = view.neuralAdmissionReason;
    }
    {
        // NR per frame for each runtime tried this session, for the runtime list.
        // Other settings change the cost, so a change there starts over.
        static std::unordered_map<std::string, float> measured;
        static std::string measuredWith;
        // What sets the cost; look values blend with the weather every frame.
        const auto& applied = view.neuralApplied;
        auto cost = std::format("{}|{}|{}|{}|{}|{}", applied.beforeUpscaling, applied.EffectivePasses(),
            applied.reconstruction.inputScale, applied.reconstruction.preset,
            static_cast<int>(applied.reconstruction.modelRate), applied.reconstruction.peripheralCompression);
        for (std::size_t i = 0; i + 1 < static_cast<std::size_t>(applied.EffectivePasses()); ++i) {
            const auto extra = applied.EffectiveExtra(i);
            cost += std::format("|{}:{}", extra.inputScale, extra.preset);
        }
        cost += std::format("|{}", static_cast<int>(applied.vulkanScheduling));
        if (cost != measuredWith) { measured.clear(); measuredWith = std::move(cost); }
        const auto& telemetry = view.neuralState.telemetry;
        if (view.neuralState.active && telemetry.HasFrame() && !view.appliedNeuralRuntimePath.empty()) {
            measured[TheosRenderPipeline::SourceDLSSG::NeuralRuntimeKey(view.appliedNeuralRuntimePath)] =
                static_cast<float>(telemetry.AverageFrameGPUMicroseconds() / 1000.0);
        }
        view.neuralRuntimeMilliseconds.clear();
        for (const auto& runtime : view.neuralRuntimes) {
            const bool automatic = TheosRenderPipeline::SourceDLSSG::IsAutomaticNeuralRuntime(runtime);
            const bool nativeApplied = view.neuralApplied.backend == TheosRenderPipeline::NeuralRendering::Backend::Native;
            const auto key = automatic ? (view.neuralApplied.automaticRuntime && nativeApplied ?
                TheosRenderPipeline::SourceDLSSG::NeuralRuntimeKey(view.appliedNeuralRuntimePath) : std::string()) :
                TheosRenderPipeline::SourceDLSSG::NeuralRuntimeKey(runtime);
            const auto found = key.empty() ? measured.end() : measured.find(key);
            view.neuralRuntimeMilliseconds.push_back(found == measured.end() ? 0.0f : found->second);
        }
        const auto hip = measured.find(TheosRenderPipeline::SourceDLSSG::NeuralRuntimeKey(
            TheosRenderPipeline::PluginPaths::ToUtf8(TheosRenderPipeline::SourceDLSSG::AmdNeuralPass::RuntimePath())));
        view.neuralHIPMilliseconds = hip == measured.end() ? 0.0f : hip->second;
        const auto vulkan = measured.find(TheosRenderPipeline::SourceDLSSG::NeuralRuntimeKey(
            TheosRenderPipeline::PluginPaths::ToUtf8(TheosRenderPipeline::NeuralRendering::VulkanNRHost::Path())));
        view.neuralVulkanMilliseconds = vulkan == measured.end() ? 0.0f : vulkan->second;
        view.neuralComputeCapability = backend.NeuralComputeCapability();
        view.neuralRuntimeKernels = TheosRenderPipeline::NeuralRendering::RuntimeKernelsSupported();
    }
#endif
#if !defined(TRP_NO_NEURAL_RENDERING)
    view.neuralObjectStatus = TheosRenderPipeline::NeuralObjects::Status();
    view.neuralObjectCurrent = TheosRenderPipeline::NeuralObjects::Current() != nullptr;
    view.neuralObjectsAvailable = TheosRenderPipeline::NeuralObjects::Available();
#endif
    view.hdr = backend.HDRState();

    const auto* performance = PerformanceTuning::GetSingleton();
    view.timingEnabled = performance->TimingEnabled();
    view.timingQuarantined = performance->GetQueryDiagnostics().quarantined;
    view.timings = performance->GetTimingSnapshot();
    view.trace = FrameTrace::GetSingleton()->GetStatus();
    view.outputRate = outputRate.Rate();

    const auto& appearance = TheosRenderPipeline::Appearance::Runtime::Get();
    view.appearance = appearance.State();
    view.weatherCatalogue = appearance.Catalogue();

    DescribeFrameView(view);
    return view;
}
