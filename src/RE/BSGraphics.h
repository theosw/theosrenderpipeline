#pragma once
#include "DirectXMath.h"
#include "SkyrimRuntime.h"

namespace BSGraphics
{
	struct alignas(16) ViewData
	{
		DirectX::XMVECTOR m_ViewUp;
		DirectX::XMVECTOR m_ViewRight;
		DirectX::XMVECTOR m_ViewDir;
		DirectX::XMMATRIX m_ViewMat;
		DirectX::XMMATRIX m_ProjMat;
		DirectX::XMMATRIX m_ViewProjMat;
		DirectX::XMMATRIX m_UnknownMat1;
		DirectX::XMMATRIX m_ViewProjMatrixUnjittered;
		DirectX::XMMATRIX m_PreviousViewProjMatrixUnjittered;
		DirectX::XMMATRIX m_ProjMatrixUnjittered;
		DirectX::XMMATRIX m_UnknownMat2;
		float             m_ViewPort[4];  // NiRect<float> { left = 0, right = 1, top = 1, bottom = 0 }
		RE::NiPoint2      m_ViewDepthRange;
		char              _pad0[0x8];
	};
	static_assert(sizeof(ViewData) == 0x250);

	struct CameraStateData
	{
		RE::NiCamera* pReferenceCamera;
		ViewData      CamViewData;
		RE::NiPoint3  PosAdjust;
		RE::NiPoint3  CurrentPosAdjust;
		RE::NiPoint3  PreviousPosAdjust;
		bool          UseJitter;
		char          _pad0[0x8];
	};
	static_assert(sizeof(CameraStateData) == 0x290);
	static_assert(offsetof(CameraStateData, UseJitter) == 0x284);

	struct State
	{
		RE::NiPointer<RE::NiSourceTexture> pDefaultTextureProjNoiseMap;
		RE::NiPointer<RE::NiSourceTexture> pDefaultTextureProjDiffuseMap;
		RE::NiPointer<RE::NiSourceTexture> pDefaultTextureProjNormalMap;
		RE::NiPointer<RE::NiSourceTexture> pDefaultTextureProjNormalDetailMap;
		char                               _pad0[0x1C];
		float                              unknown[2];
		float                              jitter[2];

		// 1.7 inserts separate UI projection scales at 0x4C/0x50 and moves
		// the frame counter/runtime data. Keep these fields behind accessors.

		struct RUNTIME_DATA
		{
			RE::NiPointer<RE::NiSourceTexture> pDefaultTextureBlack;  // "BSShader_DefHeightMap"
			RE::NiPointer<RE::NiSourceTexture> pDefaultTextureWhite;
			RE::NiPointer<RE::NiSourceTexture> pDefaultTextureGrey;
			RE::NiPointer<RE::NiSourceTexture> pDefaultHeightMap;
			RE::NiPointer<RE::NiSourceTexture> pDefaultReflectionCubeMap;
			RE::NiPointer<RE::NiSourceTexture> pDefaultFaceDetailMap;
			RE::NiPointer<RE::NiSourceTexture> pDefaultTexEffectMap;
			RE::NiPointer<RE::NiSourceTexture> pDefaultTextureNormalMap;
			RE::NiPointer<RE::NiSourceTexture> pDefaultTextureDitherNoiseMap;
			RE::BSTArray<CameraStateData>      kCameraDataCacheA;
			float                              _pad2;                  // unknown dword
			float                              fHaltonSequence[2][8];  // (2, 3) Halton Sequence points
			float                              dynamicResolutionWidthRatio;
			float                              dynamicResolutionHeightRatio;
			float                              dynamicResolutionPreviousWidthRatio;
			float                              dynamicResolutionPreviousHeightRatio;
			std::uint32_t                      dynamicResolutionIncreaseFrameWaited;
			volatile std::int32_t              dynamicResolutionLock;
			bool                               canIncreaseDynamicResolution;
			bool                               canDecreaseDynamicResolution;
			bool                               canChangeDynamicResolution;
		};
		static_assert(offsetof(RUNTIME_DATA, dynamicResolutionWidthRatio) == 0xA4);
		static_assert(offsetof(RUNTIME_DATA, dynamicResolutionLock) == 0xB8);

		[[nodiscard]] RUNTIME_DATA& GetRuntimeData() noexcept
		{
			return REL::RelocateMember<RUNTIME_DATA>(this, GetLayout().runtimeData);
		}

		[[nodiscard]] inline const RUNTIME_DATA& GetRuntimeData() const noexcept
		{
			return REL::RelocateMember<RUNTIME_DATA>(this, GetLayout().runtimeData);
		}

		[[nodiscard]] std::uint32_t GetFrameCount() const noexcept
		{
			return REL::RelocateMember<std::uint32_t>(this, GetLayout().frameCounter);
		}

	private:
		[[nodiscard]] static const TheosRenderPipeline::SkyrimRuntime::GraphicsLayout& GetLayout() noexcept
		{
			const auto* profile = TheosRenderPipeline::SkyrimRuntime::Find(REL::Module::get().version());
			if (!profile) {
				SKSE::stl::report_and_fail("No verified graphics layout for this Skyrim runtime.");
			}
			return profile->graphics;
		}
	};
	static_assert(offsetof(State, jitter) == 0x44);

}
