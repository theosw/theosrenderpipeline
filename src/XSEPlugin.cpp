#include <PCH.h>
#include "PluginPaths.h"
#include "LoggingPolicy.h"
#include "SkyrimRuntime.h"

#include "DRS.h"
#include "RenderPipeline.h"
#include "FrameTrace.h"
#include "FrameGen/SourceFrameGeneration.h"
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
#include "FrameGen/SourceNRRegression.h"
#endif
#include "PerformanceTuning.h"
#include "NvidiaBaselinePolicy.h"
#include "NativeInput.h"
#include "NativeUIBridge.h"
#include "../compatibility/ImGuiCompat/ImGuiIntegration.h"
#include "UpscalerHooks.h"
#include "CommunityShaderIntegration.h"
#include <SolFGLateOverlayAPI.h>
#include <SolFGStartupOverlayAPI.h>
#include "FrameGen/NvidiaHost.h"
#include <SimpleIni.h>
#include <process.h>

namespace
{
#if !defined(ENABLE_SKYRIM_SE) || !defined(ENABLE_SKYRIM_AE) || defined(ENABLE_SKYRIM_VR)
#error Build with both Skyrim SE and AE enabled, and VR disabled.
#endif
	void InitializeLog()
	{
		std::vector<spdlog::sink_ptr> sinks;
#ifndef NDEBUG
		sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#endif
		auto path = logger::log_directory();
		if (!path) {
			util::report_and_fail("Failed to find standard logging directory"sv);
		}
		*path /= std::format("{}.log"sv, Plugin::NAME);
		sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true));

#ifndef NDEBUG
		const auto level = spdlog::level::trace;
#else
		const auto level = spdlog::level::info;
#endif

		auto log = std::make_shared<spdlog::logger>("global log"s, sinks.begin(), sinks.end());
		log->set_level(level);
		TheosRenderPipeline::Logging::ConfigureFlush(*log);

		spdlog::set_default_logger(std::move(log));
		spdlog::flush_every(std::chrono::seconds(1));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v"s);
	}

	std::filesystem::path GetPluginDirectory()
	{
		return TheosRenderPipeline::PluginPaths::Directory();
	}

	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg && a_msg->type == SKSE::MessagingInterface::kPostLoad) {
			TheosRenderPipeline::CommunityShaders::SelectRenderer();
			if (!TheosRenderPipeline::CommunityShaders::Active()) {
				const auto runtime = GetPluginDirectory() / L"TheosRenderPipeline" / L"nvngx_dlss.dll";
				logger::info("nvngx_dlss.dll preload from \"{}\": {}", runtime.string(), ::LoadLibraryW(runtime.c_str()) ? "ok" : "failed");
				DRS::InstallHooks();
			}
			InstallUpscalerHooks();
		}
		if (!TheosRenderPipeline::CommunityShaders::Active()) { DRS::GetSingleton()->MessageHandler(a_msg); }
		RenderPipeline::GetSingleton()->MessageHandler(a_msg);
		if (a_msg && a_msg->type == SKSE::MessagingInterface::kPostLoad && !TheosRenderPipeline::CommunityShaders::Active()) {
			TheosRenderPipeline::ImGuiIntegration::Install();
		}
	}
}

namespace
{
    // Preserve the completed-scene ABI as an unavailable capability. Startup
    // foreground composition uses the separate startup-overlay API below.
    std::int32_t __cdecl QueryLateOverlayBridge(SolFGLateOverlayAPI::FrameV1*)
    {
        return 0;
    }

    std::int32_t __cdecl BeginLateOverlayBridge(SolFGLateOverlayAPI::FrameV1*)
    {
        return 0;
    }

    void __cdecl EndLateOverlayBridge() {}
}

// Optional additive query. BridgeV1 layout and legacy producer behavior stay
// unchanged; companions can retire coordinate workarounds on this native host.
extern "C" DLLEXPORT std::uint32_t __cdecl SolFG_HasNativeInputCoordinates()
{
	return TheosRenderPipeline::NativeInput::Active() ? 1u : 0u;
}

// Additive factory: the original late-overlay API retains its completed-scene
// contract. Startup consumers opt into a transparent deferred foreground.
const SolFGStartupOverlayAPI::BridgeV1* TheosRenderPipeline::NativeUIBridge::Startup(std::uint32_t version)
{
    if (version != SolFGStartupOverlayAPI::kVersion1) { return nullptr; }
    static constexpr SolFGStartupOverlayAPI::BridgeV1 bridge{
        sizeof(SolFGStartupOverlayAPI::BridgeV1), SolFGStartupOverlayAPI::kVersion1,
        +[](SolFGStartupOverlayAPI::FrameV1* frame) -> std::int32_t {
            auto* host = NvidiaHost::GetSingleton();
            if (!frame || frame->structSize < sizeof(*frame) || !host->QueryStartupOverlay()) { return 0; }
            frame->width = host->OutputWidth(); frame->height = host->OutputHeight(); frame->frameId = host->PresentCount();
            return 1;
        },
        +[](SolFGStartupOverlayAPI::FrameV1* frame) -> std::int32_t {
            auto* host = NvidiaHost::GetSingleton();
            if (!frame || frame->structSize < sizeof(*frame) || !host->BeginStartupOverlay()) { return 0; }
            frame->width = host->OutputWidth(); frame->height = host->OutputHeight(); frame->frameId = host->PresentCount();
            return 1;
        },
        +[]() { NvidiaHost::GetSingleton()->EndStartupOverlay(); }
    };
    return &bridge;
}

const SolFGLateOverlayAPI::BridgeV1* TheosRenderPipeline::NativeUIBridge::LateOverlay(std::uint32_t a_requestedVersion)
{

	static constexpr SolFGLateOverlayAPI::BridgeV1 bridge{
		sizeof(SolFGLateOverlayAPI::BridgeV1),
		SolFGLateOverlayAPI::kVersion1,
		&QueryLateOverlayBridge,
		&BeginLateOverlayBridge,
		&EndLateOverlayBridge
	};
	if (a_requestedVersion != SolFGLateOverlayAPI::kVersion1) {
		logger::error("[LateOverlayBridge] unsupported ABI requested: {}", a_requestedVersion);
		return nullptr;
	}
	return &bridge;
}

extern "C" DLLEXPORT const SolFGStartupOverlayAPI::BridgeV1* __cdecl SolFG_GetStartupOverlayBridge(std::uint32_t version)
{
    return TheosRenderPipeline::NativeUIBridge::Startup(version);
}

extern "C" DLLEXPORT const SolFGLateOverlayAPI::BridgeV1* __cdecl SolFG_GetLateOverlayBridge(std::uint32_t version)
{
    return TheosRenderPipeline::NativeUIBridge::LateOverlay(version);
}

extern "C" DLLEXPORT bool __cdecl SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	if (a_skse->IsEditor() || !TheosRenderPipeline::SkyrimRuntime::Find(a_skse->RuntimeVersion())) {
		util::report_and_fail("Theo's Render Pipeline requires Steam Skyrim 1.5.97, 1.6.640, 1.6.1170 or 1.7.104.");
	}
	InitializeLog();
	logger::info("{} v{} loading", Plugin::NAME, Plugin::VERSION_STRING);
	logger::info("[Runtime] Skyrim {}", a_skse->RuntimeVersion().string());
	SKSE::Init(a_skse);

	CSimpleIniA baselineIni;
	baselineIni.SetUnicode();
	if (baselineIni.LoadFile(L"Data\\SKSE\\Plugins\\TheosRenderPipeline.ini") < 0) {
		util::report_and_fail("Theo's Render Pipeline: SKSE/Plugins/TheosRenderPipeline.ini is missing or unreadable. Install the packaged TheosRenderPipeline.ini and restart Skyrim.");
	}
	if (const auto* error = TheosRenderPipeline::ValidateNvidiaBaseline(baselineIni)) {
		util::report_and_fail(std::format("Theo's Render Pipeline configuration error:\n\n{}\n\nCorrect SKSE/Plugins/TheosRenderPipeline.ini and restart Skyrim.", error));
	}
	logger::info("{} {}", Plugin::DISPLAY_NAME, Plugin::RELEASE_VERSION);

	// Capture the engine callee before post-load renderer hooks replace its call.
	TheosRenderPipeline::CommunityShaders::RememberEngineBoundary();

	// Load runtime paths and the initial interpolation request before the
	// required NVIDIA host is constructed during device creation.
	SourceFrameGeneration::GetSingleton()->LoadINI();
	// Resolve package-relative runtime paths from this DLL, independent of the
	// game working directory and the MO2 mod's physical installation location.
	auto& baselineSettings = SourceFrameGeneration::GetSingleton()->settings;
	baselineSettings.sourceDLSSGStreamlineDirectory = TheosRenderPipeline::ResolveRuntimePath(
		baselineSettings.sourceDLSSGStreamlineDirectory, GetPluginDirectory()).string();
	baselineSettings.neuralRenderingRuntimePath = TheosRenderPipeline::ResolveRuntimePath(
		baselineSettings.neuralRenderingRuntimePath, GetPluginDirectory()).string();
	PerformanceTuning::GetSingleton()->LoadStartupINI();
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
	TheosRenderPipeline::SourceNRRegression::LoadINI();
#endif

	SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);
    // All call hooks share this block for the lifetime of the plugin.
    // Reallocating it later can free stubs that game code still branches through.
    SKSE::AllocTrampoline(1024);
	// Select hook ownership at kPostLoad, after all SKSE plugins are loaded.

	logger::info("{} loaded", Plugin::NAME);
	return true;
}

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData v;
	v.PluginName(Plugin::NAME.data());
	v.PluginVersion(Plugin::VERSION);
	// The admission check, metadata and artwork hook use the same exact profiles.
	static_assert(TheosRenderPipeline::SkyrimRuntime::kProfiles.size() < std::size(v.compatibleVersions));
	for (std::size_t i = 0; i < TheosRenderPipeline::SkyrimRuntime::kProfiles.size(); ++i) {
		v.compatibleVersions[i] = TheosRenderPipeline::SkyrimRuntime::kProfiles[i].version.pack();
	}
	// CommonLib and our graphics wrapper select the engine layout at runtime.
	v.UsesNoStructs();
	return v;
}();

extern "C" DLLEXPORT bool __cdecl SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* pluginInfo)
{
	pluginInfo->name = SKSEPlugin_Version.pluginName;
	pluginInfo->infoVersion = SKSE::PluginInfo::kVersion;
	pluginInfo->version = SKSEPlugin_Version.pluginVersion;
	return !a_skse->IsEditor() && TheosRenderPipeline::SkyrimRuntime::Find(a_skse->RuntimeVersion());
}
