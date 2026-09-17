#include "RenderPipeline.h"

#include "DLSSBackend.h"
#include "DLSSPreset.h"
#include "DRS.h"
#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "PerformanceTuning.h"
#include "SettingsFile.h"

#include <SimpleIni.h>

void RenderPipeline::SetFrameGenerationTransitionBlocked(bool a_blocked)
{
	mFrameGenerationTransitionBlocked.store(a_blocked, std::memory_order_release);
	TheosRenderPipeline::SourceDLSSG::Backend::Get().SetTransitionBlocked(a_blocked);
}

void RenderPipeline::LoadINI()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	const auto loadResult = ini.LoadFile(L"Data\\SKSE\\Plugins\\TheosRenderPipeline.ini");
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
	mRequestLoadingArtwork.store(ini.GetBoolValue("Settings", "RequestLoadingArtwork", true), std::memory_order_relaxed);
	mWheelerLateOverlayBridge = ini.GetBoolValue("Compatibility", "WheelerLateOverlayBridge", true);
	PerformanceTuning::Settings performanceSettings{};
	performanceSettings.enableGPUTimings = ini.GetBoolValue("Performance", "EnableGPUTimings", true);
	performanceSettings.enableFrameTrace = ini.GetBoolValue("Performance", "EnableFrameTrace", false);
	performanceSettings.directRCASOutput = ini.GetBoolValue("Performance", "DirectRCASOutput", false);
	performanceSettings.directDLSSOutput = ini.GetBoolValue("Performance", "DirectDLSSOutput", false);
	PerformanceTuning::GetSingleton()->ApplySettings(performanceSettings);
	const bool unsupportedDynamicResolution = ini.GetBoolValue("DynamicResolution", "Enabled", false) ||
		ini.GetBoolValue("DynamicResolution", "Oscillate", false);
	if (unsupportedDynamicResolution) {
		logger::error(
			"[DynRes] ignored unsupported DynamicResolution request: TheosRenderPipeline's scaled-proxy path upscales at Present and requires a fixed full proxy input");
	}
	mToggleOverlayHotkey = (int)ini.GetLongValue("Hotkeys", "ToggleOverlay", 0x23);
	mLogMenuMetrics = ini.GetBoolValue("Debug", "LogMenuMetrics", false);
	mQualityLevel = std::clamp(mQualityLevel, 0, 4);

	// A reload after the feature exists (e.g. kDataLoaded) must not stomp the
	// computed optimal bias with the INI's stored value.
	if (mUseOptimalMipLodBias && DLSSBackend::GetSingleton()->HasFeature()) {
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
	if (bFXAAEnabled) {
		if (bFXAAEnabled->GetBool()) {
			logger::info("Forcing FXAA off.");
		}
		bFXAAEnabled->data.b = false;
	}
}

bool RenderPipeline::SaveINI()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	const auto loadResult = TheosRenderPipeline::SettingsFile::LoadForUpdate(ini, L"Data\\SKSE\\Plugins\\TheosRenderPipeline.ini");
	if (loadResult < 0) {
		logger::error("Could not read Data\\SKSE\\Plugins\\TheosRenderPipeline.ini before saving (rc={}); file left unchanged", static_cast<int>(loadResult));
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
    ini.SetValue("Experimental", "SourceDLSSGStreamlineDirectory", sourceSettings.sourceDLSSGStreamlineDirectory.c_str());
    ini.SetValue("Experimental", "NeuralRenderingRuntimePath", sourceSettings.neuralRenderingRuntimePath.c_str());
    frameGeneration->StoreUIComposition(ini);
    TheosRenderPipeline::SourceDLSSG::StorePreferences(ini, sourceSettings.sourceDLSSG);
	ini.SetBoolValue("Debug", "LogMenuMetrics", mLogMenuMetrics);
	const auto rc = ini.SaveFile(L"Data\\SKSE\\Plugins\\TheosRenderPipeline.ini");
	if (rc < 0) {
		logger::error("Could not save Data\\SKSE\\Plugins\\TheosRenderPipeline.ini (rc={})", static_cast<int>(rc));
		return false;
	}
	logger::info("Settings saved (rc={})", static_cast<int>(rc));
	if (sourceHost->StartupConfigured()) { sourceHost->SourceUpscalerSettingsSaved(); }
	return true;
}

void RenderPipeline::MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	static bool inited = false;
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
		RequestHistoryReset();
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

void RenderPipeline::GetJitters(float* a_outX, float* a_outY)
{
	const auto phase = DLSSBackend::GetSingleton()->GetJitterPhaseCount();

	mJitterIndex++;
	DLSSBackend::GetJitterOffset(a_outX, a_outY, static_cast<int>(mJitterIndex), phase);
}

void RenderPipeline::SetJitterOffsets(float a_x, float a_y)
{
	mJitterOffsets[0] = a_x;
	mJitterOffsets[1] = a_y;
}

void RenderPipeline::SetupDepth(ID3D11Texture2D* a_depthBuffer)
{
	if (!a_depthBuffer) {
		logger::error("SetupDepth received a null texture");
		return;
	}
	// Hold a real reference: the creator can release this texture later (menu
	// or device transitions) and NGX must never see a dangling input.
	a_depthBuffer->AddRef();
	mDepthBuffer.Release();
	mDepthBuffer.mImage = a_depthBuffer;
	logger::info("Depth buffer captured");
}

void RenderPipeline::SetupMotionVector(ID3D11Texture2D* a_motionBuffer)
{
	if (!a_motionBuffer || !mDevice || !mContext) {
		logger::error("SetupMotionVector received incomplete device/resource state");
		return;
	}
	a_motionBuffer->AddRef();
	mMotionVectors.Release();
	mMotionVectors.mImage = a_motionBuffer;
	logger::info("Motion vector buffer captured");
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
		nvidiaHost->SplitSourceDLSSActive() ? "TheosRenderPipeline-DLSS" : "unavailable",
		mRenderSizeX,
		mRenderSizeY,
		mDisplaySizeX,
		mDisplaySizeY,
		mRenderScale,
		mMipLodBias);
}
