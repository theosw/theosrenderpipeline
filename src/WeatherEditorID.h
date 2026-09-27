#pragma once

#include <cstdint>
#include <string>

namespace TheosRenderPipeline::Appearance
{
// Native EditorID Fix's public by-value ABI, also preserved by its VR port:
// github.com/vadimtrifonov/skyrim-vr-native-editorid-fix/blob/main/src/api/NativeEditorIDFixAPI.hpp
struct EditorFormInfo { std::uint32_t id; std::uint8_t type; };
static_assert(sizeof(EditorFormInfo) == 8);
using NativeEditorLookup = const char* (*)(EditorFormInfo);
// powerofthree's Tweaks public GetFormEditorID export; no hard dependency.
using TweaksEditorLookup = const char* (*)(std::uint32_t);
inline std::string EditorName(const char* native, EditorFormInfo form, NativeEditorLookup neif, TweaksEditorLookup tweaks)
{
    if (native && *native) { return native; }
    if (neif) { if (const auto* name = neif(form); name && *name) { return name; } }
    if (tweaks) { if (const auto* name = tweaks(form.id); name && *name) { return name; } }
    return {};
}
}
