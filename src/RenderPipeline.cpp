#include "RenderPipeline.h"

#include "DLSSBackend.h"
#include "DLSSPreset.h"
#include "DRS.h"
#include "CommunityShaderIntegration.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "FrameGen/NRRenderSize.h"
#include "PerformanceTuning.h"
#include "UserSettings.h"
#include "PackagedDefaults.h"
#include "OverlayHotkeys.h"
#include "OverlayLayout.h"
#include "WeatherAppearanceINI.h"
#include "WeatherAppearanceFiles.h"
#include "WeatherAppearanceRuntime.h"

#include <SimpleIni.h>

void RenderPipeline::SetFrameGenerationTransitionBlocked(bool a_blocked)
{
	mFrameGenerationTransitionBlocked.store(a_blocked, std::memory_order_release);
	TheosRenderPipeline::SourceDLSSG::Backend::Get().SetTransitionBlocked(a_blocked);
}

void RenderPipeline::RequestHistoryReset(const char* a_reason)
{
	mSceneResetReason.store(a_reason, std::memory_order_release);
	const auto generation = mSceneGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	logger::info("[Transitions] scene {} requested: {}", generation, a_reason);
}

void RenderPipeline::LoadINI()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	const auto loadResult = TheosRenderPipeline::UserSettings::Load(ini);
	if (loadResult < 0) {
		logger::warn("Could not load Data\\SKSE\\Plugins\\TheosRenderPipeline.ini (rc={}), using defaults", static_cast<int>(loadResult));
	}
	mUpscaleType = (int)ini.GetLongValue("Settings", "UpscaleType", 0);
	mQualityLevel = (int)ini.GetLongValue("Settings", "QualityLevel", 2);
	mUseOptimalMipLodBias = ini.GetBoolValue("Settings", "UseOptimalMipLodBias", true);
	mMipLodBias = (float)ini.GetDoubleValue("Settings", "MipLodBias", 0.0);
	mSharpening = ini.GetBoolValue("Settings", "Sharpening", false);
	mSharpness = (float)ini.GetDoubleValue("Settings", "Sharpness", 0.3);
	mEnableJitter = ini.GetBoolValue("Settings", "EnableJitter", true);
	mAutoExposure = ini.GetBoolValue("Settings", "AutoExposure", true);
	mDLSSPreset = TheosRenderPipeline::DLSSPreset::Sanitize((int)ini.GetLongValue("Settings", "DLSSPreset", 0));
	mNativeUI = ini.GetBoolValue("Settings", "NativeUI", true);
    mReShadeBeforeUpscaling = ini.GetBoolValue("Compatibility", "ReShadeBeforeUpscaling", false);
    mReShadeGameDevice = ini.GetBoolValue("Compatibility", "ReShadeGameDevice", false);
	mRequestLoadingArtwork.store(ini.GetBoolValue("Settings", "RequestLoadingArtwork", true), std::memory_order_relaxed);
	mWheelerLateOverlayBridge = ini.GetBoolValue("Compatibility", "WheelerLateOverlayBridge", true);
	PerformanceTuning::Settings performanceSettings{};
	performanceSettings.enableGPUTimings = ini.GetBoolValue("Performance", "EnableGPUTimings", true);
	performanceSettings.enableFrameTrace = ini.GetBoolValue("Performance", "EnableFrameTrace", false);
	performanceSettings.directRCASOutput = ini.GetBoolValue("Performance", "DirectRCASOutput", true);
	performanceSettings.directDLSSOutput = ini.GetBoolValue("Performance", "DirectDLSSOutput", true);
	PerformanceTuning::GetSingleton()->ApplySettings(performanceSettings);
	const bool unsupportedDynamicResolution = ini.GetBoolValue("DynamicResolution", "Enabled", false) ||
		ini.GetBoolValue("DynamicResolution", "Oscillate", false);
	if (unsupportedDynamicResolution) {
		logger::error(
			"[DynRes] ignored unsupported DynamicResolution request: TheosRenderPipeline's scaled-proxy path upscales at Present and requires a fixed full proxy input");
	}
	std::optional<std::string> unreadableHotkey;
	mToggleOverlayHotkey = TheosRenderPipeline::Overlay::LoadMenuHotkey(ini, &unreadableHotkey);
	if (unreadableHotkey) {
		logger::warn("[Overlay Input] ToggleOverlay=\"{}\" is not a virtual-key code (such as 0x79 for F10) or a key name (F1-F24 or a letter); using End",
			*unreadableHotkey);
	}
	mEnableNRHotkeys = TheosRenderPipeline::Overlay::LoadNRHotkeysEnabled(ini);
	logger::info("[Overlay Input] NR shortcuts enabled={}", mEnableNRHotkeys);
	// [Overlay] Language picks the dictionary the menu's labels are read from. The
	// overlay activates it before the first font atlas exists; "auto" follows the
	// Windows UI language and a missing dictionary keeps the English labels.
	mLanguage = ini.GetValue("Overlay", "Language", "auto");
	mLogMenuMetrics = ini.GetBoolValue("Debug", "LogMenuMetrics", false);
	mScreenshotDiagnostics = ini.GetBoolValue("Debug", "ScreenshotDiagnostics", false);
	mQualityLevel = std::clamp(mQualityLevel, 0, 4);
    TheosRenderPipeline::Appearance::Runtime::Get().Configure(TheosRenderPipeline::Appearance::Runtime::Load(ini));

	// A reload after the feature exists (e.g. kDataLoaded) must not stomp the
	// computed optimal bias with the INI's stored value.
    const auto* host = NvidiaHost::GetSingleton();
    if (mUseOptimalMipLodBias && host->NRFriendlyRenderSizeActive() && host->UpscalerReady()) {
        mMipLodBias = TheosRenderPipeline::NeuralRendering::ReloadNRRenderSizeMipBias(mMipLodBias,
            mUseOptimalMipLodBias, host->NRFriendlyRenderSizeActive(), host->UpscalerReady(), host->OptimalMipmapBias());
    } else if (mUseOptimalMipLodBias && DLSSBackend::GetSingleton()->HasFeature()) {
		mMipLodBias = DLSSBackend::GetSingleton()->GetOptimalMipLodBias();
	}

	NvidiaHost::GetSingleton()->AdoptEffectiveSourceUpscalerSettings();

	logger::info(
		"Settings: UpscaleType={} QualityLevel={} Sharpening={} Sharpness={:.2f} EnableJitter={}",
		mUpscaleType,
		mQualityLevel,
		mSharpening,
		mSharpness,
		mEnableJitter);

	auto bFXAAEnabled = RE::GetINISetting("bFXAAEnabled:Display");
	if (bFXAAEnabled && !TheosRenderPipeline::CommunityShaders::Active()) {
		if (bFXAAEnabled->GetBool()) {
			logger::info("Forcing FXAA off.");
		}
		bFXAAEnabled->data.b = false;
	}
}

bool RenderPipeline::SaveINI(const TheosRenderPipeline::Overlay::Layout* layout)
{
	CSimpleIniA ini;
	ini.SetUnicode();
	// Start from the defaults plus the existing user file; only differences are written back.
	SI_Error userLoad = SI_OK;
	TheosRenderPipeline::UserSettings::Load(ini, TheosRenderPipeline::UserSettings::GamePaths(), &userLoad);
	if (userLoad < 0) {
		logger::error("Could not read TheosRenderPipeline.User.ini before saving (rc={}); file left unchanged", static_cast<int>(userLoad));
		return false;
	}
	auto* sourceHost = NvidiaHost::GetSingleton();
	const auto creation = sourceHost->StartupConfigured() ?
		sourceHost->SourceUpscalerSettings().Requested() :
		TheosRenderPipeline::Upscaler::Creation{mUpscaleType, mQualityLevel, mDLSSPreset, mSharpening, mAutoExposure};
	ini.SetLongValue("Settings", "UpscaleType", creation.mode);
	ini.SetLongValue("Settings", "QualityLevel", creation.quality);
	ini.SetBoolValue("Settings", "UseOptimalMipLodBias", mUseOptimalMipLodBias);
	ini.SetDoubleValue("Settings", "MipLodBias", mMipLodBias);
	ini.SetBoolValue("Settings", "Sharpening", creation.sharpening);
	ini.SetDoubleValue("Settings", "Sharpness", mSharpness);
	ini.SetBoolValue("Settings", "EnableJitter", mEnableJitter);
	ini.SetBoolValue("Settings", "AutoExposure", creation.autoExposure);
	ini.SetLongValue("Settings", "DLSSPreset", creation.preset);
	ini.SetBoolValue("Settings", "NativeUI", mNativeUI);
    ini.SetBoolValue("Compatibility", "ReShadeBeforeUpscaling", mReShadeBeforeUpscaling);
    ini.SetBoolValue("Compatibility", "ReShadeGameDevice", mReShadeGameDevice);
	ini.SetBoolValue("Settings", "RequestLoadingArtwork", mRequestLoadingArtwork.load(std::memory_order_relaxed));
	ini.SetBoolValue("Compatibility", "WheelerLateOverlayBridge", mWheelerLateOverlayBridge);
	const auto& performanceSettings = PerformanceTuning::GetSingleton()->settings;
	ini.SetBoolValue("Performance", "EnableGPUTimings", performanceSettings.enableGPUTimings);
	ini.SetBoolValue("Performance", "EnableFrameTrace", performanceSettings.enableFrameTrace);
	ini.SetBoolValue("Performance", "DirectRCASOutput", performanceSettings.directRCASOutput);
	ini.SetBoolValue("Performance", "DirectDLSSOutput", performanceSettings.directDLSSOutput);
    // Preserve unrecognized and retired research keys from the loaded INI.
    const auto* frameGeneration = SourceFrameGeneration::GetSingleton();
    ini.SetBoolValue("FrameGeneration", "Enabled", frameGeneration->RuntimeInterpolationRequested());
    const auto& sourceSettings = frameGeneration->settings;
    frameGeneration->StoreRuntimePaths(ini);
    frameGeneration->StoreUIComposition(ini);
    frameGeneration->StoreCompatibilityPreference(ini);
    TheosRenderPipeline::SourceDLSSG::StorePreferences(ini, sourceSettings.sourceDLSSG);
    // Preset files first; the main INI's appearance settings follow only once every file is written.
    auto& appearance = TheosRenderPipeline::Appearance::Runtime::Get();
    const auto presets = TheosRenderPipeline::Appearance::SaveAppearance(ini, appearance.Configuration(),
        TheosRenderPipeline::Appearance::Runtime::PresetFolder());
    for (const auto& error : presets.errors) { logger::error("[Appearance] {}", error); }
    logger::info("[Appearance] saved {} preset files ({} unchanged)", presets.files, presets.unchanged);
    appearance.Configure(presets.settings);
    const bool presetsSaved = presets.Ok();
	ini.SetBoolValue("Debug", "LogMenuMetrics", mLogMenuMetrics);
	TheosRenderPipeline::Overlay::StoreMenuHotkey(ini, mToggleOverlayHotkey);
	ini.SetValue("Overlay", "Language", mLanguage.c_str());
    if (layout) { TheosRenderPipeline::Overlay::StoreLayout(ini, *layout); }
	std::vector<std::string> changed;
	if (!TheosRenderPipeline::UserSettings::Save(ini, TheosRenderPipeline::UserSettings::GamePaths(),
			TheosRenderPipeline::UserSettings::PackagedDefaults, &changed)) {
		logger::error("Could not save Data\\SKSE\\Plugins\\TheosRenderPipeline.User.ini");
		return false;
	}
	logger::info("Settings saved to TheosRenderPipeline.User.ini ({} saved overrides)", changed.size());
	if (sourceHost->StartupConfigured()) { sourceHost->SourceUpscalerSettingsSaved(); }
	return presetsSaved;
}

void RenderPipeline::MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	static bool inited = false;
    if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
        TheosRenderPipeline::Appearance::Runtime::Get().CaptureCatalogue();
    }
    if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame || a_msg->type == SKSE::MessagingInterface::kNewGame) {
        TheosRenderPipeline::Appearance::Runtime::Get().Invalidate();
    }
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kDataLoaded:
	case SKSE::MessagingInterface::kNewGame:
	case SKSE::MessagingInterface::kPreLoadGame:
		if (!inited) {
			LoadINI();
			inited = true;
		}
		break;
	case SKSE::MessagingInterface::kPostLoadGame:
		// The save is actually in the world now; flush DLSS history so the
		// first visible frames do not blend with loading-screen content.
		// Also the harness's definitive world-entry marker.
		logger::info("[AutoTest] world entered (kPostLoadGame)");
		RequestHistoryReset("save loaded");
		// No further auto-load attempts once a save is in.
		break;
	}
}

void RenderPipeline::SetupSwapChain(IDXGISwapChain* a_swapChain)
{
	mSwapChain = a_swapChain;
	mSwapChain->GetDevice(IID_PPV_ARGS(&mDevice));
	mDevice->GetImmediateContext(&mContext);
	DLSSBackend::GetSingleton()->SetupDevice(mDevice, mContext);
}

bool RenderPipeline::IsEnabled()
{
	return !DRS::GetSingleton()->reset;
}

int RenderPipeline::JitterPhaseCount() const
{
	const auto* host = NvidiaHost::GetSingleton();
    return host->XeSSActive() && host->RenderWidth() && host->RenderHeight() ?
        static_cast<int>(std::ceil(8.0 * host->OutputWidth() * host->OutputHeight() / (static_cast<double>(host->RenderWidth()) * host->RenderHeight()))) :
        DLSSBackend::GetSingleton()->GetJitterPhaseCount();
}

void RenderPipeline::GetJitters(float* a_outX, float* a_outY)
{
	mJitterIndex++;
	DLSSBackend::GetJitterOffset(a_outX, a_outY, static_cast<int>(mJitterIndex & 0x3FFFFFFFu), JitterPhaseCount());
}

void RenderPipeline::SetJitterOffsets(float a_x, float a_y)
{
	mJitterOffsets[0] = a_x;
	mJitterOffsets[1] = a_y;
}

void RenderPipeline::PreInit()
{
	LoadINI();
	ID3D11Texture2D* backBuffer = nullptr;
	mSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
	if (backBuffer) {
		D3D11_TEXTURE2D_DESC desc;
		backBuffer->GetDesc(&desc);
		mDisplaySizeX = desc.Width;
		mDisplaySizeY = desc.Height;
		backBuffer->Release();
		logger::info("Display size: {} x {}", mDisplaySizeX, mDisplaySizeY);
	}
}

void RenderPipeline::InitUpscaler()
{
	auto* nvidiaHost = NvidiaHost::GetSingleton();
	// Device creation has already completed the required NVIDIA host.
	mDisplaySizeX = static_cast<int>(nvidiaHost->OutputWidth());
	mDisplaySizeY = static_cast<int>(nvidiaHost->OutputHeight());
	mRenderSizeX = static_cast<int>(nvidiaHost->RenderWidth());
	mRenderSizeY = static_cast<int>(nvidiaHost->RenderHeight());
	mRenderScale = static_cast<float>(mRenderSizeX) /
		static_cast<float>(mDisplaySizeX > 0 ? mDisplaySizeX : 1);
	if (mUseOptimalMipLodBias) {
		mMipLodBias = nvidiaHost->OptimalMipmapBias();
	}
	SetGameTAA(false);
	if (auto rendererData = RE::BSGraphics::Renderer::GetRendererDataSingleton()) {
		// Keep the actual borderless-window authority native between render
		// calls. UpscalerHooks scopes this cache down to the render extent only
		// while Skyrim is calculating world/UI layout. Leaving it reduced here
		// lets focus restoration resize the real HWND to the DLSS input size.
		rendererData->renderWindows[0].windowWidth = mDisplaySizeX;
		rendererData->renderWindows[0].windowHeight = mDisplaySizeY;
		logger::info(
			"[NvidiaHost] window authority kept native {}x{}; render extent {}x{} is scoped to draw entry points",
			mDisplaySizeX,
			mDisplaySizeY,
			mRenderSizeX,
			mRenderSizeY);
	}
	logger::info(
		"NVIDIA host topology initialized: sourceOwner={} {} x {} -> {} x {} (scale {:.3f}, mip bias {:.2f})",
		nvidiaHost->XeSSActive() ? "TheosRenderPipeline-XeSS" : nvidiaHost->SplitSourceDLSSActive() ? "TheosRenderPipeline-DLSS" : "unavailable",
		mRenderSizeX,
		mRenderSizeY,
		mDisplaySizeX,
		mDisplaySizeY,
		mRenderScale,
		mMipLodBias);
}
