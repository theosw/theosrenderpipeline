#include "DLSSBackend.h"
#include "DLSSPreset.h"
#include "DLSSFeatureParameters.h"

#include <PCH.h>
#include "PluginPaths.h"

#include "PerformanceTuning.h"

#include <cmath>

#include <d3dcompiler.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_helpers.h>

namespace
{
	// TheosRenderPipeline's own NGX project id (random UUID, not shared with any other title).
	constexpr const char* kNGXProjectId = "f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11";

	const char* NGXResultToString(NVSDK_NGX_Result a_result)
	{
		switch (a_result) {
		case NVSDK_NGX_Result_Success:
			return "Success";
		case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
			return "FeatureNotSupported";
		case NVSDK_NGX_Result_FAIL_PlatformError:
			return "PlatformError";
		case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:
			return "FeatureAlreadyExists";
		case NVSDK_NGX_Result_FAIL_FeatureNotFound:
			return "FeatureNotFound";
		case NVSDK_NGX_Result_FAIL_InvalidParameter:
			return "InvalidParameter";
		case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:
			return "ScratchBufferTooSmall";
		case NVSDK_NGX_Result_FAIL_NotInitialized:
			return "NotInitialized";
		case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:
			return "UnsupportedInputFormat";
		case NVSDK_NGX_Result_FAIL_RWFlagMissing:
			return "RWFlagMissing";
		case NVSDK_NGX_Result_FAIL_MissingInput:
			return "MissingInput";
		case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
			return "UnableToInitializeFeature";
		case NVSDK_NGX_Result_FAIL_OutOfDate:
			return "OutOfDate";
		case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:
			return "OutOfGPUMemory";
		case NVSDK_NGX_Result_FAIL_UnsupportedFormat:
			return "UnsupportedFormat";
		case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath:
			return "UnableToWriteToAppDataPath";
		case NVSDK_NGX_Result_FAIL_UnsupportedParameter:
			return "UnsupportedParameter";
		case NVSDK_NGX_Result_FAIL_Denied:
			return "Denied";
		case NVSDK_NGX_Result_FAIL_NotImplemented:
			return "NotImplemented";
		default:
			return "Unknown";
		}
	}

	bool NGXSucceeded(NVSDK_NGX_Result a_result)
	{
		return NVSDK_NGX_SUCCEED(a_result);
	}

	void NVSDK_CONV LogNGXCallback(const char* a_message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
	{
		if (a_message) {
			logger::info("[NGX] {}", a_message);
		}
	}

	std::filesystem::path GetPluginDirectory()
	{
		return TheosRenderPipeline::PluginPaths::Directory();
	}

	std::filesystem::path GetNGXDataPath()
	{
		std::error_code ec;
		auto path = std::filesystem::temp_directory_path(ec);
		if (ec) {
			return {};
		}
		path /= L"TheosRenderPipeline";
		std::filesystem::create_directories(path, ec);
		return path;
	}

	float Halton(int a_index, int a_base)
	{
		float f = 1.0f;
		float r = 0.0f;
		while (a_index > 0) {
			f /= static_cast<float>(a_base);
			r += f * static_cast<float>(a_index % a_base);
			a_index /= a_base;
		}
		return r;
	}

}

void DLSSBackend::SetupDevice(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
{
	if (device && device != a_device) {
		ReleaseDirectDestinationUAV();
	}
	device = a_device;
	context = a_context;
	logger::info("[DLSSBackend] device set: device={} context={}", reinterpret_cast<void*>(device), reinterpret_cast<void*>(context));
}

bool DLSSBackend::EnsureNGXInitialized()
{
	if (ngxInitialized) {
		return true;
	}
	if (ngxInitAttempted) {
		return false;
	}
	ngxInitAttempted = true;

	if (!device) {
		logger::error("[DLSSBackend] cannot initialize NGX without a D3D11 device");
		return false;
	}

	const auto dataPath = GetNGXDataPath();
	// nvngx_dlss.dll ships in Data/SKSE/Plugins/TheosRenderPipeline next to the plugin DLL.
	const auto modulePath = GetPluginDirectory() / L"TheosRenderPipeline";
	const auto modulePathString = modulePath.wstring();
	const wchar_t* moduleSearchPath = modulePathString.c_str();

	NVSDK_NGX_FeatureCommonInfo featureInfo{};
	featureInfo.PathListInfo.Path = &moduleSearchPath;
	featureInfo.PathListInfo.Length = 1;
	featureInfo.LoggingInfo.LoggingCallback = LogNGXCallback;
	featureInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
	featureInfo.LoggingInfo.DisableOtherLoggingSinks = false;

	const auto result = NVSDK_NGX_D3D11_Init_with_ProjectID(
		kNGXProjectId,
		NVSDK_NGX_ENGINE_TYPE_CUSTOM,
		Plugin::VERSION_STRING.data(),
		dataPath.c_str(),
		device,
		&featureInfo,
		NVSDK_NGX_Version_API);

	ngxInitialized = NGXSucceeded(result);
	logger::info(
		"[DLSSBackend] NVSDK_NGX_D3D11_Init_with_ProjectID result=0x{:08X} ({}) searchPath=\"{}\" dataPath=\"{}\"",
		static_cast<uint32_t>(result),
		NGXResultToString(result),
		modulePath.string(),
		dataPath.string());
	return ngxInitialized;
}

bool DLSSBackend::EnsureCapabilityParameters()
{
	if (capabilityParams) {
		return true;
	}

	NVSDK_NGX_Parameter* params = nullptr;
	const auto result = NVSDK_NGX_D3D11_GetCapabilityParameters(&params);
	if (!NGXSucceeded(result) || !params) {
		logger::error("[DLSSBackend] NVSDK_NGX_D3D11_GetCapabilityParameters result=0x{:08X} ({})", static_cast<uint32_t>(result), NGXResultToString(result));
		return false;
	}
	capabilityParams = params;

	int available = 0;
	NVSDK_NGX_Parameter_GetI(capabilityParams, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	dlssAvailable = available != 0;

	int needsDriverUpdate = 0;
	NVSDK_NGX_Parameter_GetI(capabilityParams, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriverUpdate);

	logger::info("[DLSSBackend] DLSS available={} needsDriverUpdate={}", dlssAvailable, needsDriverUpdate != 0);
	return true;
}

bool DLSSBackend::IsDLSSAvailable()
{
	if (!EnsureNGXInitialized() || !EnsureCapabilityParameters()) {
		return false;
	}
	return dlssAvailable;
}

void DLSSBackend::ReleaseFeature()
{
	// Startup, resize and live replacement all enter after host retirement.
	if (dlssHandle) { NVSDK_NGX_D3D11_ReleaseFeature(dlssHandle); }
	ReleaseDirectDestinationUAV();
	if (outputSRV) { outputSRV->Release(); }
	if (sharpenUAV) { sharpenUAV->Release(); }
	if (sharpenTexture) { sharpenTexture->Release(); }
	if (outputTexture) { outputTexture->Release(); }
	dlssHandle = nullptr;
	outputTexture = nullptr;
	sharpenTexture = nullptr;
	outputSRV = nullptr;
	sharpenUAV = nullptr;
}

bool DLSSBackend::InitUpscale(
	int a_renderWidth,
	int a_renderHeight,
	int a_targetWidth,
	int a_targetHeight,
	DXGI_FORMAT a_inputFormat,
	bool a_sharpening,
	bool a_autoExposure,
	int a_preset,
	int a_qualityLevel)
{
	if (!device || !context) {
		logger::error("[DLSSBackend] InitUpscale called before SetupDevice");
		return false;
	}
	if (!IsDLSSAvailable()) {
		logger::error("[DLSSBackend] DLSS is not available on this system");
		return false;
	}

	displayWidth = a_targetWidth;
	displayHeight = a_targetHeight;
	renderWidth = a_renderWidth;
	renderHeight = a_renderHeight;
	// Quality 5 denotes the host's native DLAA allocation.
	isDLAA = a_qualityLevel == 5;
	// The host supplies the quality that produced its published render extent.
	qualityLevel = std::clamp(a_qualityLevel, 0, 5);
	sharpening = a_sharpening;
	autoExposure = a_autoExposure;
	preset = a_preset;
	inputFormat = a_inputFormat;

	return CreateFeature();
}

bool DLSSBackend::CreateFeature()
{
	ReleaseFeature();

	// Render preset hints (NVSDK_NGX_DLSS_Hint_Render_Preset values; 0 =
	// driver default, 5/6 = legacy CNN presets E/F, 10/11 = transformer
	// presets J/K, 12/13 = L/M). Applied to every quality slot so the choice follows mode
	// switches.
	const auto presetValue = static_cast<unsigned int>(TheosRenderPipeline::DLSSPreset::Sanitize(preset));
	NVSDK_NGX_Parameter_SetUI(capabilityParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, presetValue);
	NVSDK_NGX_Parameter_SetUI(capabilityParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, presetValue);
	NVSDK_NGX_Parameter_SetUI(capabilityParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, presetValue);
	NVSDK_NGX_Parameter_SetUI(capabilityParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, presetValue);
	NVSDK_NGX_Parameter_SetUI(capabilityParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, presetValue);
	NVSDK_NGX_Parameter_SetUI(capabilityParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, presetValue);

	auto createParams = TheosRenderPipeline::DLSS::CreationParameters(renderWidth, renderHeight,
		displayWidth, displayHeight, inputFormat, qualityLevel, sharpening, autoExposure);

	const auto createResult = NGX_D3D11_CREATE_DLSS_EXT(context, &dlssHandle, capabilityParams, &createParams);
	logger::info(
		"[DLSSBackend] NGX_D3D11_CREATE_DLSS_EXT result=0x{:08X} ({}) handle={} render={}x{} display={}x{} quality={} flags=0x{:08X}",
		static_cast<uint32_t>(createResult),
		NGXResultToString(createResult),
		reinterpret_cast<void*>(dlssHandle),
		renderWidth,
		renderHeight,
		displayWidth,
		displayHeight,
		static_cast<int>(createParams.Feature.InPerfQualityValue),
		static_cast<uint32_t>(createParams.InFeatureCreateFlags));
	if (!NGXSucceeded(createResult) || !dlssHandle) {
		dlssHandle = nullptr;
		return false;
	}

	logger::info(
		"[DLSSBackend] initialized: mode={} renderScale={:.3f} isHDR={} preset={} fixedInput=true",
		isDLAA ? "DLAA" : "DLSS",
		static_cast<float>(renderWidth) / static_cast<float>(displayWidth),
		IsHDRInput(),
		preset);
	return true;
}

bool DLSSBackend::IsHDRInput() const
{
	return TheosRenderPipeline::DLSS::IsHDRFormat(inputFormat);
}

bool DLSSBackend::EnsureOutputTexture(ID3D11Texture2D* a_destination)
{
	D3D11_TEXTURE2D_DESC destDesc{};
	a_destination->GetDesc(&destDesc);

	if (outputTexture) {
		D3D11_TEXTURE2D_DESC haveDesc{};
		outputTexture->GetDesc(&haveDesc);
		if (haveDesc.Format == destDesc.Format && haveDesc.Width == destDesc.Width && haveDesc.Height == destDesc.Height) {
			return true;
		}
		if (outputSRV) {
			outputSRV->Release();
			outputSRV = nullptr;
		}
		if (sharpenUAV) {
			sharpenUAV->Release();
			sharpenUAV = nullptr;
		}
		if (sharpenTexture) {
			sharpenTexture->Release();
			sharpenTexture = nullptr;
		}
		outputTexture->Release();
		outputTexture = nullptr;
	}

	observedOutputFormat = destDesc.Format;

	// DLSS writes through a UAV, and game render targets are not always
	// UAV-capable, so the feature renders into this backend-owned texture which
	// Evaluate then copies to the caller's destination (formats must match for
	// CopyResource, hence cloning the destination's format).
	D3D11_TEXTURE2D_DESC outputDesc{};
	outputDesc.Width = destDesc.Width;
	outputDesc.Height = destDesc.Height;
	outputDesc.MipLevels = 1;
	outputDesc.ArraySize = 1;
	outputDesc.Format = destDesc.Format;
	outputDesc.SampleDesc.Count = 1;
	outputDesc.Usage = D3D11_USAGE_DEFAULT;
	outputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	const auto outputResult = device->CreateTexture2D(&outputDesc, nullptr, &outputTexture);
	if (FAILED(outputResult) || !outputTexture) {
		logger::error("[DLSSBackend] failed to create output texture hr=0x{:08X} format={} {}x{}", static_cast<uint32_t>(outputResult), static_cast<int>(destDesc.Format), destDesc.Width, destDesc.Height);
		return false;
	}
	// Companion resources for the RCAS pass.
	if (FAILED(device->CreateShaderResourceView(outputTexture, nullptr, &outputSRV)) ||
		FAILED(device->CreateTexture2D(&outputDesc, nullptr, &sharpenTexture)) ||
		FAILED(device->CreateUnorderedAccessView(sharpenTexture, nullptr, &sharpenUAV))) {
		logger::warn("[DLSSBackend] RCAS resources unavailable; sharpening disabled");
	}

	logger::info("[DLSSBackend] output texture created: {}x{} format={}", destDesc.Width, destDesc.Height, static_cast<int>(destDesc.Format));
	return true;
}

void DLSSBackend::ReleaseDirectDestinationUAV()
{
	if (directDestinationUAV) {
		directDestinationUAV->Release();
		directDestinationUAV = nullptr;
	}
	directDestination = nullptr;
}

bool DLSSBackend::EnsureDirectDestinationUAV(ID3D11Texture2D* a_destination)
{
	if (!a_destination || !device) {
		return false;
	}
	if (directDestination == a_destination && directDestinationUAV) {
		return true;
	}
	ReleaseDirectDestinationUAV();

	D3D11_TEXTURE2D_DESC desc{};
	a_destination->GetDesc(&desc);
	const bool compatible =
		desc.Width == static_cast<UINT>(displayWidth) &&
		desc.Height == static_cast<UINT>(displayHeight) &&
		desc.MipLevels == 1 && desc.ArraySize == 1 &&
		desc.SampleDesc.Count == 1 &&
		desc.Usage == D3D11_USAGE_DEFAULT &&
		(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
	if (!compatible) {
		return false;
	}
	const auto hr = device->CreateUnorderedAccessView(a_destination, nullptr, &directDestinationUAV);
	if (FAILED(hr) || !directDestinationUAV) {
		logger::warn(
			"[Performance] direct destination UAV creation failed hr=0x{:08X} format={} {}x{} bind=0x{:X}",
			static_cast<std::uint32_t>(hr),
			static_cast<int>(desc.Format),
			desc.Width,
			desc.Height,
			desc.BindFlags);
		return false;
	}
	directDestination = a_destination;
	logger::info(
		"[Performance] native destination accepted for direct output: {}x{} format={} bind=0x{:X}",
		desc.Width,
		desc.Height,
		static_cast<int>(desc.Format),
		desc.BindFlags);
	return true;
}

bool DLSSBackend::EnsureExposureTexture()
{
	if (exposureTexture) {
		return true;
	}
	const float one = 1.0f;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = 1;
	desc.Height = 1;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA init{ &one, sizeof(float), 0 };
	if (FAILED(device->CreateTexture2D(&desc, &init, &exposureTexture))) {
		logger::error("[DLSSBackend] exposure texture creation failed");
		return false;
	}
	return true;
}

bool DLSSBackend::EnsureRCAS(float a_sharpness)
{
	// Coarse quantization: slider drags recompile per frame otherwise.
	if (rcasShader && std::abs(a_sharpness - rcasCompiledSharpness) < 0.025f) {
		return true;
	}

	const auto shaderPath = GetPluginDirectory() / L"TheosRenderPipeline\\RCAS.hlsl";
	const auto sharpnessValue = std::format("{:.3f}", a_sharpness);
	const D3D_SHADER_MACRO macros[]{ { "SHARPNESS", sharpnessValue.c_str() }, { nullptr, nullptr } };

	ID3DBlob* blob = nullptr;
	ID3DBlob* errors = nullptr;
	if (FAILED(D3DCompileFromFile(shaderPath.c_str(), macros, nullptr, "main", "cs_5_0", 0, 0, &blob, &errors))) {
		logger::error("[DLSSBackend] RCAS compile failed: {}", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
		if (errors) {
			errors->Release();
		}
		return false;
	}
	if (errors) {
		errors->Release();
	}

	ID3D11ComputeShader* compiled = nullptr;
	const auto hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &compiled);
	blob->Release();
	if (FAILED(hr) || !compiled) {
		logger::error("[DLSSBackend] RCAS shader creation failed hr=0x{:08X}", static_cast<uint32_t>(hr));
		return false;
	}
	if (rcasShader) {
		rcasShader->Release();
	}
	rcasShader = compiled;
	rcasCompiledSharpness = a_sharpness;
	logger::info("[DLSSBackend] RCAS compiled (sharpness {:.3f})", a_sharpness);
	return true;
}

bool DLSSBackend::Evaluate(
	ID3D11Texture2D* a_color,
	ID3D11Texture2D* a_motionVectors,
	ID3D11Texture2D* a_depth,
	ID3D11Texture2D* a_destination,
	int a_renderWidth,
	int a_renderHeight,
	float a_sharpness,
	float a_jitterX,
	float a_jitterY,
	float a_motionScaleX,
	float a_motionScaleY,
	bool a_reset)
{
	if (!context || !dlssHandle || !capabilityParams) {
		return false;
	}
	if (!a_color || !a_motionVectors || !a_depth || !a_destination) {
		return false;
	}

	// Observation is diagnostic; the host's allocation supplies creation flags.
	D3D11_TEXTURE2D_DESC colorDesc{};
	a_color->GetDesc(&colorDesc);
	if (observedInputFormat != colorDesc.Format) {
		observedInputFormat = colorDesc.Format;
		logger::info("[DLSSBackend] observed color input format={}", static_cast<int>(colorDesc.Format));
	}

	if (!EnsureOutputTexture(a_destination)) {
		return false;
	}
	auto performance = PerformanceTuning::GetSingleton();
	const bool sharpeningRequested = a_sharpness > 0.01f;
	bool useDirectDLSSOutput = false;
	if (performance->IsRouteAllowed(PerformanceTuning::Optimization::kDirectDLSSOutput)) {
		if (sharpeningRequested) {
			performance->MarkRouteWaiting(
				PerformanceTuning::Optimization::kDirectDLSSOutput,
				"armed; activates when sharpening is off");
		} else if (EnsureDirectDestinationUAV(a_destination)) {
			performance->MarkRouteEligible(
				PerformanceTuning::Optimization::kDirectDLSSOutput,
				"native destination is UAV-capable and extent-matched");
			useDirectDLSSOutput = true;
		} else {
			performance->MarkRouteFallback(
				PerformanceTuning::Optimization::kDirectDLSSOutput,
				"native destination is not a compatible NGX output",
				true);
		}
	}
	if (performance->IsRouteAllowed(PerformanceTuning::Optimization::kDirectRCASOutput) && !sharpeningRequested) {
		performance->MarkRouteWaiting(
			PerformanceTuning::Optimization::kDirectRCASOutput,
			"armed; activates when sharpening is on");
	}

	NVSDK_NGX_D3D11_DLSS_Eval_Params evalParams{};
	evalParams.Feature.pInColor = a_color;
	evalParams.Feature.pInOutput = useDirectDLSSOutput ? a_destination : outputTexture;
	evalParams.Feature.InSharpness = a_sharpness;
	evalParams.pInDepth = a_depth;
	evalParams.pInMotionVectors = a_motionVectors;
	evalParams.InJitterOffsetX = a_jitterX;
	evalParams.InJitterOffsetY = a_jitterY;
	evalParams.InRenderSubrectDimensions.Width = static_cast<unsigned int>(a_renderWidth);
	evalParams.InRenderSubrectDimensions.Height = static_cast<unsigned int>(a_renderHeight);
	evalParams.InReset = a_reset ? 1 : 0;
	evalParams.InMVScaleX = a_motionScaleX;
	evalParams.InMVScaleY = a_motionScaleY;
	evalParams.InPreExposure = 1.0f;
	evalParams.InExposureScale = 1.0f;
	// Without the AutoExposure create-flag, NGX requires an exposure input
	// (crash observed in the driver when it was missing).
	if (!autoExposure && EnsureExposureTexture()) {
		evalParams.pInExposureTexture = exposureTexture;
	}

	NVSDK_NGX_Result result{};
	{
		ScopedD3D11PerformanceStage timer{ context, PerformanceTuning::D3D11Stage::kDLSS };
		result = NGX_D3D11_EVALUATE_DLSS_EXT(context, dlssHandle, capabilityParams, &evalParams);
		if (!NGXSucceeded(result) && useDirectDLSSOutput) {
			performance->MarkRouteFallback(
				PerformanceTuning::Optimization::kDirectDLSSOutput,
				"NGX rejected the shared native target; intermediate-output retry selected",
				true);
			useDirectDLSSOutput = false;
			evalParams.Feature.pInOutput = outputTexture;
			evalParams.InReset = 1;
			result = NGX_D3D11_EVALUATE_DLSS_EXT(context, dlssHandle, capabilityParams, &evalParams);
		}
	}
	lastEvalResult = static_cast<uint32_t>(result);
	static uint64_t consecutiveFails = 0;
	if (NGXSucceeded(result)) {
		++evalSuccessCount;
		if (consecutiveFails > 0) {
			logger::info("[DLSSBackend] eval recovered after {} consecutive failures", consecutiveFails);
			consecutiveFails = 0;
		}
	} else {
		++evalFailCount;
		++consecutiveFails;
	}
	static bool loggedFirstResult = false;
	const bool logThisResult =
		!loggedFirstResult ||
		(!NGXSucceeded(result) && (consecutiveFails == 1 || consecutiveFails % 300 == 0));
	if (logThisResult) {
		loggedFirstResult = true;
		logger::info(
			"[DLSSBackend] NGX_D3D11_EVALUATE_DLSS_EXT result=0x{:08X} ({}) render={}x{} jitter=({:.4f},{:.4f}) mvScale=({:.1f},{:.1f}) reset={} consecutiveFails={}",
			static_cast<uint32_t>(result),
			NGXResultToString(result),
			a_renderWidth,
			a_renderHeight,
			a_jitterX,
			a_jitterY,
			a_motionScaleX,
			a_motionScaleY,
			a_reset,
			consecutiveFails);
		if (!NGXSucceeded(result) && device) {
			D3D11_TEXTURE2D_DESC destDesc{};
			a_destination->GetDesc(&destDesc);
			logger::info(
				"[DLSSBackend] eval-fail context: deviceRemovedReason=0x{:08X} dest={}x{} fmt={} hasOutput={} hasExposure={} sharpness={:.2f}",
				static_cast<uint32_t>(device->GetDeviceRemovedReason()),
				destDesc.Width,
				destDesc.Height,
				static_cast<int>(destDesc.Format),
				outputTexture != nullptr,
				evalParams.pInExposureTexture != nullptr,
				a_sharpness);
		}
	}
	if (!NGXSucceeded(result)) {
		return false;
	}
	if (useDirectDLSSOutput) {
		performance->MarkRouteActive(PerformanceTuning::Optimization::kDirectDLSSOutput);
	}

	// Optional RCAS sharpening pass between the DLSS output and the
	// destination (the DLSS built-in InSharpness is a driver no-op).
	if (sharpeningRequested && outputSRV && sharpenUAV && EnsureRCAS(a_sharpness)) {
		bool useDirectRCASOutput = false;
		ID3D11UnorderedAccessView* targetUAV = sharpenUAV;
		if (performance->IsRouteAllowed(PerformanceTuning::Optimization::kDirectRCASOutput)) {
			if (EnsureDirectDestinationUAV(a_destination)) {
				performance->MarkRouteEligible(
					PerformanceTuning::Optimization::kDirectRCASOutput,
					"native destination is UAV-capable and extent-matched");
				targetUAV = directDestinationUAV;
				useDirectRCASOutput = true;
			} else {
				performance->MarkRouteFallback(
					PerformanceTuning::Optimization::kDirectRCASOutput,
					"native destination UAV contract was rejected",
					true);
			}
		}
		ID3D11ShaderResourceView* srvs[]{ outputSRV };
		ID3D11UnorderedAccessView* uavs[]{ targetUAV };
		{
			ScopedD3D11PerformanceStage timer{ context, PerformanceTuning::D3D11Stage::kRCAS };
			context->CSSetShader(rcasShader, nullptr, 0);
			context->CSSetShaderResources(0, 1, srvs);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->Dispatch((displayWidth + 7) / 8, (displayHeight + 7) / 8, 1);
			ID3D11ShaderResourceView* nullSRV[]{ nullptr };
			ID3D11UnorderedAccessView* nullUAV[]{ nullptr };
			context->CSSetShaderResources(0, 1, nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
		}
		if (useDirectRCASOutput) {
			performance->MarkRouteActive(PerformanceTuning::Optimization::kDirectRCASOutput);
		} else {
			ScopedD3D11PerformanceStage timer{ context, PerformanceTuning::D3D11Stage::kOutputCopy };
			context->CopyResource(a_destination, sharpenTexture);
		}
	} else if (!useDirectDLSSOutput) {
		if (sharpeningRequested && performance->IsRouteAllowed(PerformanceTuning::Optimization::kDirectRCASOutput)) {
			performance->MarkRouteFallback(
				PerformanceTuning::Optimization::kDirectRCASOutput,
				"RCAS shader resources are unavailable; original unsharpened output selected",
				true);
		}
		ScopedD3D11PerformanceStage timer{ context, PerformanceTuning::D3D11Stage::kOutputCopy };
		context->CopyResource(a_destination, outputTexture);
	}
	return true;
}

float DLSSBackend::GetOptimalMipLodBias() const
{
	if (isDLAA || renderWidth == 0 || displayWidth == 0 || renderWidth == displayWidth) {
		return 0.0f;
	}
	// DLSS programming guide: lod bias = log2(render / display) - 1.
	return std::log2(static_cast<float>(renderWidth) / static_cast<float>(displayWidth)) - 1.0f;
}

int DLSSBackend::GetJitterPhaseCount() const
{
	if (renderWidth == 0 || displayWidth == 0) {
		return 8;
	}
	const auto ratio = static_cast<float>(displayWidth) / static_cast<float>(renderWidth);
	return static_cast<int>(std::ceil(8.0f * ratio * ratio));
}

void DLSSBackend::GetJitterOffset(float* a_outX, float* a_outY, int a_index, int a_phaseCount)
{
	if (a_phaseCount < 1) {
		a_phaseCount = 1;
	}
	const auto sequenceIndex = (a_index % a_phaseCount) + 1;
	if (a_outX) {
		*a_outX = Halton(sequenceIndex, 2) - 0.5f;
	}
	if (a_outY) {
		*a_outY = Halton(sequenceIndex, 3) - 0.5f;
	}
}
