#pragma once

#include "OverlayPresetDecor.h"
#include "OverlayUIStyle.h"
#include <algorithm>
#include <cfloat>
#include <format>
#include <initializer_list>
#include <string>

namespace TheosRenderPipeline::Overlay
{
// Lays out items left to right, wrapping when the next one would not fit.
class Flow
{
public:
    Flow() : right_(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x) {}
    void Next(float width)
    {
        if (!first_ && lastEnd_ + ImGui::GetStyle().ItemSpacing.x + width <= right_) { ImGui::SameLine(); }
        first_ = false;
        lastEnd_ = ImGui::GetCursorPosX() + width;
    }
    bool Checkbox(const char* label, bool* value)
    {
        Next(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label, nullptr, true).x);
        return ImGui::Checkbox(label, value);
    }
    bool Button(const char* label)
    {
        Next(ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2);
        return ImGui::Button(label);
    }
    void Text(const char* text)
    {
        Next(ImGui::CalcTextSize(text).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", text);
    }
private:
    float right_, lastEnd_{};
    bool first_{true};
};

inline float LabelWidth(std::initializer_list<const char*> labels)
{
    float width = 0;
    for (const auto* label : labels) { width = (std::max)(width, ImGui::CalcTextSize(label).x); }
    return width + ImGui::GetStyle().ItemSpacing.x * 3;
}
inline ImU32 ChangedTint() { return ImGui::GetColorU32(ImVec4(kAmber.x, kAmber.y, kAmber.z, 0.13f)); }

namespace Rows
{
// Narrow windows show only Reset; its tooltip gives Base's value.
inline bool compactState{};
// Widths in 1x units; callers multiply by the menu size.
inline constexpr float MaxControl = 520, MinControl = 140, WideState = 260, NarrowState = 90, MaxPassCell = 360;
inline float StateWidth(float available, float fixed)
{
    compactState = available < fixed + Px(WideState);
    return ActivePresetDecor() ? Px(compactState ? NarrowState : WideState) : 0;
}
inline void Label(const char* label, bool changed, bool restarts)
{
    ImGui::AlignTextToFramePadding();
    if (changed) { ImGui::TextColored(kAmber, "%s", label); } else { ImGui::TextUnformatted(label); }
    if (restarts) {
        ImGui::SameLine(0, Px(5.0f));
        ImGui::TextColored(kOchre, "!");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", RestartsNRHelp); }
    }
}
}

// Label left, control right. Presets add a state column: "Base", or Base's value and Reset.
inline bool BeginSettingRows(const char* id, float labelWidth)
{
    const float available = ImGui::GetContentRegionAvail().x;
    const float state = Rows::StateWidth(available, labelWidth + Px(Rows::MaxControl));
    const float control = std::clamp(available - labelWidth - state - ImGui::GetStyle().CellPadding.x * 6,
        Px(Rows::MinControl), Px(Rows::MaxControl));
    if (!ImGui::BeginTable(id, ActivePresetDecor() ? 3 : 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) { return false; }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, control);
    if (ActivePresetDecor()) { ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch); }
    return true;
}
// Whether any of keys is changed by the preset being edited.
inline bool AnyChanged(std::initializer_list<const char*> keys)
{
    const auto* decor = ActivePresetDecor();
    return decor && std::ranges::any_of(keys, [&](const char* key) { return key && decor->Changed(key); });
}
// The state cell for one row: Base, or what changed with a Reset.
inline void StateCell(std::initializer_list<const char*> keys)
{
    auto* decor = ActivePresetDecor();
    if (!decor) { return; }
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    std::string base;
    for (const auto* key : keys) {
        if (key && decor->Changed(key)) { base += std::format("{}{}", base.empty() ? "" : ", ", decor->BaseValue(key)); }
    }
    if (base.empty()) { ImGui::TextDisabled("Base"); return; }
    // Rows disable only their label and control, so Reset stays usable.
    if (!Rows::compactState) { ImGui::TextDisabled("was %s in Base", base.c_str()); ImGui::SameLine(); }
    ImGui::PushID(*keys.begin());
    if (ImGui::SmallButton("Reset")) {
        for (const auto* key : keys) { if (key && decor->Changed(key)) { decor->Reset(key); } }
    }
    ImGui::PopID();
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Use Base's value (%s).", base.c_str()); }
}
// disabled greys the label and control that cannot apply now; the state cell stays usable.
template <class Control>
bool SettingRow(const char* label, const char* key, bool restarts, bool disabled, Control&& control)
{
    const bool changed = AnyChanged({key});
    ImGui::TableNextRow();
    if (changed) { ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ChangedTint()); }
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(disabled);
    Rows::Label(label, changed, restarts);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    const bool result = control();
    ImGui::PopID();
    ImGui::EndDisabled();
    StateCell({key});
    return result;
}

// Per-pass settings side by side: label, Pass 1, Pass 2, then presets' state column.
inline bool BeginPassTable(const char* id, float labelWidth)
{
    const float available = ImGui::GetContentRegionAvail().x;
    const float state = Rows::StateWidth(available, labelWidth + 2 * Px(Rows::MaxPassCell));
    const float cell = std::clamp((available - labelWidth - state - ImGui::GetStyle().CellPadding.x * 8) / 2,
        Px(Rows::MinControl), Px(Rows::MaxPassCell));
    if (!ImGui::BeginTable(id, ActivePresetDecor() ? 4 : 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) { return false; }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
    ImGui::TableSetupColumn("pass1", ImGuiTableColumnFlags_WidthFixed, cell);
    ImGui::TableSetupColumn("pass2", ImGuiTableColumnFlags_WidthFixed, cell);
    if (ActivePresetDecor()) { ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch); }
    return true;
}
// One per-pass row. control(pass) draws that pass's widget. Pass 2 greys out while it is off.
template <class Control>
bool PassRow(const char* label, const char* setting, bool restarts, bool disabled, bool pass2Enabled, Control&& control)
{
    const auto key1 = std::string("Pass1") + setting, key2 = std::string("Pass2") + setting;
    const bool changed1 = AnyChanged({key1.c_str()}), changed2 = AnyChanged({key2.c_str()});
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(disabled);
    Rows::Label(label, changed1 || changed2, restarts);
    bool result = false;
    ImGui::PushID(label);
    for (int pass = 0; pass < 2; ++pass) {
        ImGui::TableNextColumn();
        if (pass ? changed2 : changed1) { ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ChangedTint()); }
        if (pass) { ImGui::BeginDisabled(!pass2Enabled); }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::PushID(pass);
        result |= control(pass);
        ImGui::PopID();
        if (pass) { ImGui::EndDisabled(); }
    }
    ImGui::PopID();
    ImGui::EndDisabled();
    StateCell({key1.c_str(), key2.c_str()});
    return result;
}
}
