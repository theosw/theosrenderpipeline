#pragma once

namespace TheosRenderPipeline::Overlay
{
// The preset editor draws the Neural Rendering controls with per-setting
// markers. Base drawing has no decor, so these calls do nothing there.
class PresetDecor
{
public:
    virtual ~PresetDecor() = default;
    // Called after the control for a preset setting key.
    virtual void Mark(const char* key) = 0;
    // Called once in the Look section, where time-of-day values apply.
    virtual void LookControls() = 0;
};
inline PresetDecor*& ActivePresetDecor()
{
    static PresetDecor* decor{};
    return decor;
}
inline void MarkSetting(const char* key)
{
    if (auto* decor = ActivePresetDecor()) { decor->Mark(key); }
}
inline void PresetLookControls()
{
    if (auto* decor = ActivePresetDecor()) { decor->LookControls(); }
}
inline constexpr const char* RestartsNRHelp =
    " (!) Changing this restarts NR briefly. A preset that changes it can hitch when the weather changes.";
}
