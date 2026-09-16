#pragma once

#include <cstdint>
#include <optional>

namespace TheosRenderPipeline::ImGuiCompat
{
	struct CursorExtent
	{
		std::uint32_t width{}, height{};
		constexpr bool Valid() const { return width > 0 && height > 0 && width <= 16384 && height <= 16384; }
	};

	struct CursorMapping
	{
		CursorExtent target;
		float scaleX, scaleY;
		bool usesClientExtent;
	};

	// The NVIDIA host already keeps late UI native until Present. Its inactive
	// legacy draw bridge must not prevent mapping Skyrim's render-space cursor
	// to the client extent used by Vanity's own ImGui Win32 backend.
	inline std::optional<CursorMapping> ResolveCursorMapping(
		CursorExtent render, CursorExtent bridge, CursorExtent client)
	{
		const bool usesClient = !bridge.Valid();
		const auto target = usesClient ? client : bridge;
		if (!render.Valid() || !target.Valid()) {
			return std::nullopt;
		}
		const float scaleX = static_cast<float>(target.width) / static_cast<float>(render.width);
		const float scaleY = static_cast<float>(target.height) / static_cast<float>(render.height);
		if (scaleX < 0.25F || scaleX > 4.0F || scaleY < 0.25F || scaleY > 4.0F) {
			return std::nullopt;
		}
		return CursorMapping{ target, scaleX, scaleY, usesClient };
	}
}
