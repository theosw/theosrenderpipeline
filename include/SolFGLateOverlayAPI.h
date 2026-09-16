#pragma once

#include <cstdint>

// Stable C ABI used by late D3D11 overlays that need to draw into the renderer's
// already-upscaled native present source. Consumers discover the bridge with
// GetProcAddress; they do not link the renderer or depend on plugin load order.
namespace SolFGLateOverlayAPI
{
	inline constexpr std::uint32_t kVersion1 = 1;

	struct FrameV1
	{
		std::uint32_t structSize{ sizeof(FrameV1) };
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint64_t frameId{ 0 };
	};

	struct BridgeV1
	{
		std::uint32_t structSize;
		std::uint32_t version;
		std::int32_t(__cdecl* query)(FrameV1* a_frame);
		std::int32_t(__cdecl* begin)(FrameV1* a_frame);
		void(__cdecl* end)();
	};

	using GetBridge = const BridgeV1*(__cdecl*)(std::uint32_t a_requestedVersion);
}
