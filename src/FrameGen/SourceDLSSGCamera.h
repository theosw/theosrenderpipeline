#pragma once
#include <sl_consts.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace BSGraphics { struct State; }
namespace TheosRenderPipeline::SourceDLSSG
{
	// This size query does not initialize
	// NGX or create a feature, which matters during D3D11 factory bootstrap.
	inline bool QueryRenderSize(int width, int height, int quality, int* renderWidth, int* renderHeight)
	{
		if (width <= 0 || height <= 0 || !renderWidth || !renderHeight || quality < 0 || quality > 5) { return false; }
		constexpr float scales[]{ 0.5f, 0.58f, 0.6666667f, 0.33333334f, 0.7777778f, 1.0f };
		*renderWidth = static_cast<int>(std::round(static_cast<float>(width) * scales[quality]));
		*renderHeight = static_cast<int>(std::round(static_cast<float>(height) * scales[quality]));
		return *renderWidth > 0 && *renderHeight > 0;
	}

	struct CameraHistory
	{
		DirectX::XMFLOAT4X4 previous{};
		std::uintptr_t identity{};
		std::uint32_t frame{};
		bool valid{};
		void Reset() { valid = false; }
		bool Build(const DirectX::XMFLOAT4X4& projection, const DirectX::XMFLOAT4X4& view,
			const sl::float3& position, float nearPlane, float farPlane,
			float jitterX, float jitterY, std::uintptr_t cameraIdentity, std::uint32_t gameFrame,
			bool reset, sl::Constants& result);
	};
	bool CaptureCameraConstants(BSGraphics::State* state, unsigned width, unsigned height,
		float jitterX, float jitterY, bool reset, bool jittered, sl::Constants& result, bool commit = true);
	// External producers can build a candidate before restoring their camera
	// state, then retain that history only when its completed frame is accepted.
	bool CaptureCameraCandidate(BSGraphics::State* state, unsigned width, unsigned height,
		float jitterX, float jitterY, bool reset, bool jittered, sl::Constants& result, CameraHistory& candidate);
}
