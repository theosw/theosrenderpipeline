#pragma once

// Game-side upscaler integration derived from PureDark's MIT-licensed
// Skyrim-Upscaler (https://github.com/PureDark/Skyrim-Upscaler).
// DLSSBackend owns NVIDIA NGX evaluation.

#include <PCH.h>
#include "UpscaleType.h"

#include <RE/BSGraphics.h>
#include <d3d11.h>
#include <dxgi.h>

// Owner of the game's TAA toggle; identified and named by the upstream
// Skyrim-Upscaler project.
struct UnkOuterStruct
{
	struct UnkInnerStruct
	{
		uint8_t unk00[0x18];  // 00
		bool    bTAA;         // 18
	};

	uint8_t         unk00[0x1F0];    // 00
	UnkInnerStruct* unkInnerStruct;  // 1F0

	// May return nullptr early in startup (e.g. during InitD3D on 1.6.1170);
	// always go through SetGameTAA/GetGameTAA below.
	static UnkOuterStruct* GetSingleton()
	{
		REL::Relocation<UnkOuterStruct*&> instance{ REL::VariantID(527731, 414660, 0x34234C0) };  // 31D11A0, 326B280, 34234C0
		return instance.get();
	}
};

inline void SetGameTAA(bool a_enabled)
{
	auto singleton = UnkOuterStruct::GetSingleton();
	if (!singleton || !singleton->unkInnerStruct) {
		return;
	}
	singleton->unkInnerStruct->bTAA = a_enabled;
}

inline bool GetGameTAA()
{
	auto singleton = UnkOuterStruct::GetSingleton();
	if (!singleton || !singleton->unkInnerStruct) {
		return false;
	}
	return singleton->unkInnerStruct->bTAA;
}

struct ImageWrapper
{
	ID3D11Texture2D* mImage{ nullptr };

	void Release()
	{
		if (mImage) {
			mImage->Release();
			mImage = nullptr;
		}
	}
};

class RenderPipeline
{
public:
	float mJitterIndex{ 0 };
	float mJitterOffsets[2]{ 0, 0 };
	bool  mSharpening{ false };
	float mSharpness{ 0.3f };
	bool  mEnableJitter{ true };
	int   mDisplaySizeX{ 0 };
	int   mDisplaySizeY{ 0 };
	int   mRenderSizeX{ 0 };
	int   mRenderSizeY{ 0 };
	float mRenderScale{ 1.0f };

	int   mUpscaleType{ 0 };
	int   mQualityLevel{ 2 };
	float mMipLodBias{ 0 };

	// Ultrawide investigation: log stage/viewport/scale metrics for every
	// menu that opens ([Debug] LogMenuMetrics).
	bool mLogMenuMetrics{ false };
	// Session-only MagicMenu preview draw isolation. Zero submits every draw;
	// the developer menu exposes individual include/exclude modes.
	int mInventory3DDrawIsolationMode{ 0 };
	std::atomic_uint32_t mInventory3DLastObservedDraws{ 0 };
	std::atomic_uint32_t mInventory3DLastSkippedDraws{ 0 };
	bool mUseOptimalMipLodBias{ true };
	bool mAutoExposure{ true };
	int  mDLSSPreset{ 0 };  // NGX preset: 0 default, 5/6 E/F, 10/11 J/K, 12/13 L/M
	bool mNativeUI{ true };  // scaled mode: rasterize the game's UI pass at native resolution
	std::atomic_bool mRequestLoadingArtwork{ true };
	bool mWheelerLateOverlayBridge{ true };  // enables the startup-overlay target handoff
	int  mToggleOverlayHotkey{ 0x23 };  // VK_END
	std::atomic_bool mConsoleOpen{ false };
	// Single relaxed hot-path gate derived from LogMenuMetrics && ConsoleOpen.
	// OM/viewport/scissor hooks do not need two conditionals or acquire loads.
	std::atomic_bool mConsoleDiagnosticsActive{ false };
	std::atomic_uint64_t mConsoleDiagnosticGeneration{ 0 };

	// Captured by the jitter hook each frame for NVIDIA camera constants.
	BSGraphics::State* mGraphicsState{ nullptr };

	// Frames the game rendered through the pre-UI hook (diverges from the
	// presented frame count once frame generation exists).
	uint64_t mRenderedFrameCount{ 0 };
	// Frames left that should evaluate with the DLSS reset flag (history
	// flush after loading screens, where stale accumulation looks wrong).
	// Window is wide because the fader can close while the loading screen is
	// still evaluating, burning reset frames before the world is visible.
	int mPendingHistoryResets{ 0 };

	void RequestHistoryReset() { mPendingHistoryResets = 15; }
	void SetFrameGenerationTransitionBlocked(bool a_blocked);
	bool FrameGenerationTransitionBlocked() const
	{
		return mFrameGenerationTransitionBlocked.load(std::memory_order_acquire);
	}

	std::atomic_bool mFrameGenerationTransitionBlocked{ true };

	ImageWrapper         mDepthBuffer;
	ImageWrapper         mMotionVectors;

	IDXGISwapChain*      mSwapChain{ nullptr };
	ID3D11Device*        mDevice{ nullptr };
	ID3D11DeviceContext* mContext{ nullptr };

	static RenderPipeline* GetSingleton()
	{
		static RenderPipeline handler;
		return &handler;
	}

	void SetupSwapChain(IDXGISwapChain* a_swapChain);

	void LoadINI();
	// Returns false when the live MO2-projected INI could not be written. The
	// overlay surfaces this result so persistence failures are never silent.
	bool SaveINI();
	void MessageHandler(SKSE::MessagingInterface::Message* a_msg);

	bool IsEnabled();

	void GetJitters(float* a_outX, float* a_outY);
	void SetJitterOffsets(float a_x, float a_y);

	void SetupDepth(ID3D11Texture2D* a_depthBuffer);
	void SetupMotionVector(ID3D11Texture2D* a_motionBuffer);
	void PreInit();
	void InitUpscaler();
};
