#pragma once
#include <algorithm>
#include <array>

namespace TheosRenderPipeline::DLSSPreset
{
	struct Entry { int value; const char* shortName; const char* label; };
	// NGX preset values from include/nvsdk_ngx_defs.h.
	inline constexpr std::array<Entry, 7> entries{{
		{ 0, "DRIVER", "Driver default" }, { 5, "E", "E | Legacy CNN" },
		{ 6, "F", "F | Legacy CNN" }, { 10, "J", "J | Transformer" },
		{ 11, "K", "K | Transformer" }, { 12, "L", "L | Transformer" },
		{ 13, "M", "M | Transformer" }
	}};
	constexpr const Entry* Find(int value)
	{
		for (const auto& entry : entries) { if (entry.value == value) { return &entry; } }
		return nullptr;
	}
	constexpr const char* ShortName(int value) { const auto* entry = Find(value); return entry ? entry->shortName : "CUSTOM"; }
	constexpr const char* Label(int value) { const auto* entry = Find(value); return entry ? entry->label : "Custom INI preset"; }
	// Preserve the existing numeric INI range and unknown choices on Apply.
	constexpr int Sanitize(int value) { return std::clamp(value, 0, 15); }
}
