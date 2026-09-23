#pragma once
#include <algorithm>
#include <array>

namespace TheosRenderPipeline::DLSSPreset
{
	struct Entry { int value; const char* shortName; const char* label; };
	// NGX preset values from include/nvsdk_ngx_defs.h.
	inline constexpr std::array<Entry, 7> entries{{
		{ 0, "DRIVER", "Driver default" }, { 5, "E", "E | Legacy / deprecated" },
		{ 6, "F", "F | Legacy / deprecated" }, { 10, "J", "J | Less ghosting, more flicker" },
		{ 11, "K", "K | DLAA / Balanced / Quality" }, { 12, "L", "L | Ultra Performance" },
		{ 13, "M", "M | Performance" }
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
    // Paraphrased NVIDIA guidance, checked 2026-09-21:
    // https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_dlss.h
    constexpr const char* Description(int value)
    {
        switch (value) {
        case 0: return "Runtime-selected model; NVIDIA updates may change the selection.";
        case 5: case 6: return "Legacy preset, deprecated in current NVIDIA guidance. Behavior depends on the runtime version.";
        case 10: return "Similar to K, with potentially less ghosting but more flicker. NVIDIA generally favors K.";
        case 11: return "Transformer preset used by default for DLAA, Balanced and Quality; prioritizes image quality at additional processing cost.";
        case 12: return "Default for Ultra Performance. Improves sharpness, stability and ghosting over J/K, at greater processing cost.";
        case 13: return "Default for Performance. Offers improvements similar to L while running nearer the speed of J/K.";
        default: return "Custom numeric INI preset; support and behavior depend on the installed NVIDIA runtime.";
        }
    }
}
