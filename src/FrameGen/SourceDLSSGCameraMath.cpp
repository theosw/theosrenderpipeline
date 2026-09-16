#include "SourceDLSSGCamera.h"
#include "SourceDLSSGSession.h"

namespace TheosRenderPipeline::SourceDLSSG
{
	bool CameraHistory::Build(const DirectX::XMFLOAT4X4& projection, const DirectX::XMFLOAT4X4& view,
		const sl::float3& position, float nearPlane, float farPlane, float jitterX, float jitterY,
		std::uintptr_t cameraIdentity, std::uint32_t gameFrame, bool reset, sl::Constants& result)
	{
		using namespace DirectX;
		auto finite = [](const XMFLOAT4X4& m) {
			for (const auto& row : m.m) { for (float v : row) { if (!std::isfinite(v)) { return false; } } }
			return true;
		};
		if (!cameraIdentity || !finite(projection) || !finite(view) ||
			std::abs(projection._34) < 0.5f || std::abs(projection._44) > 0.001f ||
			projection._11 <= 0 || projection._22 <= 0) { Reset(); return false; }
		// Engine view translation is camera-relative. Rebuild only translation
		// from the actual NiCamera position, retaining the engine's basis. Our
		// history then uses one absolute coordinate system across origin shifts.
		auto absoluteView = view;
		absoluteView._41 = -(position.x * view._11 + position.y * view._21 + position.z * view._31);
		absoluteView._42 = -(position.x * view._12 + position.y * view._22 + position.z * view._32);
		absoluteView._43 = -(position.x * view._13 + position.y * view._23 + position.z * view._33);
		absoluteView._14 = absoluteView._24 = absoluteView._34 = 0; absoluteView._44 = 1;
		const auto p = XMLoadFloat4x4(&projection);
		const auto v = XMLoadFloat4x4(&absoluteView);
		const auto vp = v * p;
		XMVECTOR determinant;
		const auto inverseVP = XMMatrixInverse(&determinant, vp);
		if (!std::isfinite(XMVectorGetX(determinant)) || std::abs(XMVectorGetX(determinant)) < 1e-12f) { Reset(); return false; }
		const bool discontinuity = reset || !valid || identity != cameraIdentity || gameFrame != frame + 1;
		const auto previousVP = discontinuity ? vp : XMLoadFloat4x4(&previous);
		const auto clipToPrevious = inverseVP * previousVP;
		auto copy = [](sl::float4x4& to, FXMMATRIX from) {
			XMFLOAT4X4 packed; XMStoreFloat4x4(&packed, from);
			static_assert(sizeof(packed) == sizeof(to));
			std::memcpy(&to, &packed, sizeof(to));
		};
		result = sl::Constants{};
		copy(result.cameraViewToClip, p);
		copy(result.clipToCameraView, XMMatrixInverse(nullptr, p));
		copy(result.clipToLensClip, XMMatrixIdentity());
		copy(result.clipToPrevClip, clipToPrevious);
		copy(result.prevClipToClip, XMMatrixInverse(nullptr, clipToPrevious));
		result.cameraPos = position;
		result.cameraRight = { view._11, view._21, view._31 };
		result.cameraUp = { view._12, view._22, view._32 };
		const float direction = projection._34 > 0 ? 1.0f : -1.0f;
		result.cameraFwd = { direction * view._13, direction * view._23, direction * view._33 };
		result.cameraNear = nearPlane; result.cameraFar = farPlane;
		result.cameraFOV = 2.0f * std::atan(1.0f / projection._22);
		result.cameraAspectRatio = projection._22 / projection._11;
		result.jitterOffset = { jitterX, jitterY };
		result.mvecScale = { 1, 1 }; // Skyrim's guides already contain normalized screen motion.
		result.cameraPinholeOffset = { 0, 0 };
		result.depthInverted = sl::eFalse;
		result.cameraMotionIncluded = sl::eTrue;
		result.motionVectors3D = sl::eFalse;
		result.reset = discontinuity ? sl::eTrue : sl::eFalse;
		if (!Session::ValidConstants(result)) { Reset(); return false; }
		XMStoreFloat4x4(&previous, vp);
		identity = cameraIdentity; frame = gameFrame; valid = true;
		return true;
	}
}
