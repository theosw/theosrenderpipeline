#pragma once

#include <cstdint>
#include <string>

namespace TheosRenderPipeline::Overlay
{
// The preset editor draws the Neural Rendering controls with per-setting state,
// compared with Base; Pass 2 is compared with Pass 1 the same way. Drawing with
// no decor shows no state column.
class PresetDecor
{
public:
    virtual ~PresetDecor() = default;
    virtual bool Changed(const char* key) const = 0;
    virtual std::string BaseValue(const char* key) const = 0;
    virtual void Reset(const char* key) = 0;
    // The time of day being edited, or null when the preset looks the same all day.
    virtual const char* EditingTime() const = 0;
    // What unchanged rows follow, as the state column names it.
    virtual const char* Reference() const { return "Base"; }
    // The reference's pass count, so the tabs of passes a preset adds are tinted; 0 when unknown.
    virtual int ReferencePasses() const { return 0; }
};
inline PresetDecor*& ActivePresetDecor()
{
    static PresetDecor* decor{};
    return decor;
}
// While Base is edited, says which weather override replaces a setting now, or
// returns an empty string when Base's value is the one in use.
class OverrideNotes
{
public:
    virtual ~OverrideNotes() = default;
    virtual std::string Note(const char* key) const = 0;
};
// The weather override being edited, or 0 for Base. Inline editors key their
// state by it so an edit in progress cannot land in another override.
inline std::uint32_t& ActiveEditScope()
{
    static std::uint32_t scope{};
    return scope;
}
inline const OverrideNotes*& ActiveOverrideNotes()
{
    static const OverrideNotes* notes{};
    return notes;
}
// translated where shown (OverlaySettingRows.h)
inline constexpr const char* RestartsNRHelp = "Changing this restarts NR briefly. A weather override that changes it "
                                              "can hitch when the weather changes.";
}
