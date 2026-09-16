#pragma once

#include <d3d11.h>
#include <dxgi.h>

// D3D11 DLSS Super Resolution backend using the public NVIDIA NGX SDK.
// The interface retains the game integration's existing settings contract.
class DLSSBackend
{
public:
	static DLSSBackend* GetSingleton()
	{
		static DLSSBackend backend;
		return &backend;
	}

	// Quality values preserve the existing INI format:
	// 0 = Performance, 1 = Balanced, 2 = Quality, 3 = Ultra Performance,
	// 4 = Ultra Quality, 5 = native DLAA on the explicit-size source path.
	void SetupDevice(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	// Creates DLSS for the host's published extents and input format. Replacing
	// an existing feature requires host GPU retirement before this call.
	bool InitUpscale(
		int a_renderWidth,
		int a_renderHeight,
		int a_targetWidth,
		int a_targetHeight,
		DXGI_FORMAT a_inputFormat,
		bool a_sharpening,
		bool a_autoExposure,
		int a_preset,
		int a_qualityLevel = 2);

	// The host must retire all GPU users before releasing the feature/output.
	void ReleaseFeature();

	// Runs DLSS on the host's render-sized inputs and publishes the native
	// result to a_destination.
	bool Evaluate(
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
		bool a_reset);

	int RenderWidth() const { return renderWidth; }
	int RenderHeight() const { return renderHeight; }

	// Diagnostics for the in-game overlay.
	uint64_t EvalSuccessCount() const { return evalSuccessCount; }
	uint64_t EvalFailCount() const { return evalFailCount; }
	uint32_t LastEvalResult() const { return lastEvalResult; }
	bool HasFeature() const { return dlssHandle != nullptr; }
	int InputFormat() const { return static_cast<int>(observedInputFormat); }
	int OutputFormat() const { return static_cast<int>(observedOutputFormat); }
	bool IsHDRInput() const;

	bool IsDLSSAvailable();

	float GetOptimalMipLodBias() const;
	int GetJitterPhaseCount() const;
	static void GetJitterOffset(float* a_outX, float* a_outY, int a_index, int a_phaseCount);

private:
	DLSSBackend() = default;
	DLSSBackend(const DLSSBackend&) = delete;
	DLSSBackend& operator=(const DLSSBackend&) = delete;

	bool EnsureNGXInitialized();
	bool EnsureCapabilityParameters();
	bool CreateFeature();
	bool EnsureOutputTexture(ID3D11Texture2D* a_destination);
	bool EnsureDirectDestinationUAV(ID3D11Texture2D* a_destination);
	void ReleaseDirectDestinationUAV();
	bool EnsureRCAS(float a_sharpness);

	ID3D11Device* device{ nullptr };
	ID3D11DeviceContext* context{ nullptr };

	bool ngxInitAttempted{ false };
	bool ngxInitialized{ false };
	bool dlssAvailable{ false };

	struct NVSDK_NGX_Parameter* capabilityParams{ nullptr };
	struct NVSDK_NGX_Handle* dlssHandle{ nullptr };

	ID3D11Texture2D* outputTexture{ nullptr };

	// 1x1 exposure texture (1.0): NGX requires an exposure input when the
	// feature is created without the AutoExposure flag.
	ID3D11Texture2D* exposureTexture{ nullptr };
	bool EnsureExposureTexture();

	// RCAS sharpening pass (the DLSS built-in sharpener is a driver no-op).
	ID3D11ShaderResourceView* outputSRV{ nullptr };
	ID3D11Texture2D* sharpenTexture{ nullptr };
	ID3D11UnorderedAccessView* sharpenUAV{ nullptr };
	ID3D11ComputeShader* rcasShader{ nullptr };
	float rcasCompiledSharpness{ -1.0f };
	ID3D11Texture2D* directDestination{ nullptr };
	ID3D11UnorderedAccessView* directDestinationUAV{ nullptr };

	int displayWidth{ 0 };
	int displayHeight{ 0 };
	int renderWidth{ 0 };
	int renderHeight{ 0 };
	bool isDLAA{ false };

	// Feature creation parameters describe the current host allocation.
	int qualityLevel{ 2 };
	bool sharpening{ false };
	bool autoExposure{ true };
	int preset{ 0 };
	DXGI_FORMAT inputFormat{ DXGI_FORMAT_UNKNOWN };

	DXGI_FORMAT observedInputFormat{ DXGI_FORMAT_UNKNOWN };
	DXGI_FORMAT observedOutputFormat{ DXGI_FORMAT_UNKNOWN };

	uint64_t evalSuccessCount{ 0 };
	uint64_t evalFailCount{ 0 };
	uint32_t lastEvalResult{ 0 };

};
