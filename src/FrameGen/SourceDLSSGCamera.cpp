#include "SourceDLSSGCamera.h"
#include <PCH.h>
#include "../RE/BSGraphics.h"

namespace TheosRenderPipeline::SourceDLSSG
{
	bool CaptureCameraConstants(BSGraphics::State* state, unsigned width, unsigned height,
		float jitterX, float jitterY, bool reset, bool jittered, sl::Constants& result, bool commit)
{
		static CameraHistory history;
		// Early NR needs camera discontinuities before DLSS without consuming
		// history if reconstruction fails. The normal post-DLSS call commits it.
		auto candidate = history;
		static std::string lastStatus;
		auto unavailable = [&](const char* reason) {
			if (commit) { history.Reset(); }
			if (lastStatus != reason) { lastStatus = reason; logger::warn("[SourceDLSSG] camera unavailable: {}", reason); }
			return false;
		};
		auto* player = RE::PlayerCamera::GetSingleton();
		if (!state || !width || !height || !player || !player->cameraRoot) { return unavailable("player camera or graphics state absent"); }
		const BSGraphics::CameraStateData* selected = nullptr;
		for (const auto& entry : state->GetRuntimeData().kCameraDataCacheA) {
			// The cache can contain both jittered and unjittered versions of the
			// same camera. Use the variant that produced the current raster guides.
			if (!entry.pReferenceCamera || entry.UseJitter != jittered) { continue; }
			// Select the actual player camera, not a shadow or reflection view.
			auto* parent = entry.pReferenceCamera->parent;
			for (unsigned depth = 0; parent && depth < 8; ++depth, parent = parent->parent) {
				if (parent == player->cameraRoot.get()) {
					if (selected) { return unavailable("multiple matching player views"); }
					selected = &entry;
					break;
				}
			}
		}
		if (!selected) { return unavailable("no matching player view in graphics cache"); }
		DirectX::XMFLOAT4X4 projection, view;
		DirectX::XMStoreFloat4x4(&projection, selected->CamViewData.m_ProjMatrixUnjittered);
		DirectX::XMStoreFloat4x4(&view, selected->CamViewData.m_ViewMat);
		const auto* camera = selected->pReferenceCamera;
		const auto& position = camera->world.translate;
		const float aspect = static_cast<float>(width) / static_cast<float>(height);
		if (projection._11 <= 0 || std::abs(projection._22 / projection._11 - aspect) > 0.02f) {
			return unavailable("projection aspect disagrees with raster extent");
		}
		const bool built = candidate.Build(projection, view, { position.x, position.y, position.z },
			camera->viewFrustum.fNear, camera->viewFrustum.fFar, jitterX, jitterY,
			reinterpret_cast<std::uintptr_t>(camera), state->uiFrameCount, reset, result);
		if (!built) { return unavailable("invalid camera constants"); }
		if (commit) { history = candidate; }
		if (lastStatus != "ready") {
			lastStatus = "ready";
			logger::info("[SourceDLSSG] player camera ready jittered={} near={} far={} fov={} aspect={}",
				jittered, result.cameraNear, result.cameraFar, result.cameraFOV, result.cameraAspectRatio);
		}
		return true;
	}
}
