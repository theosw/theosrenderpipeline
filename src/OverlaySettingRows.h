#pragma once

#include "OverlayPresetDecor.h"
#include "Localization.h"
#include "OverlayUIStyle.h"
#include "FrameGen/SourceDLSSGNeuralState.h"
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
    // gap is the space between items; zero uses the style's item spacing.
    explicit Flow(float gap = 0)
        : right_(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x), gap_(gap > 0 ? gap : ImGui::GetStyle().ItemSpacing.x) {}
    void Next(float width)
    {
        if (!first_ && lastEnd_ + gap_ + width <= right_) { ImGui::SameLine(0, gap_); }
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
    float right_, gap_, lastEnd_{};
    bool first_{true};
};

inline float LabelWidth(std::initializer_list<const char*> labels)
{
    float width = 0;
    for (const auto* label : labels) { width = (std::max)(width, ImGui::CalcTextSize(label).x); }
    return width + ImGui::GetStyle().ItemSpacing.x * 3;
}
inline ImU32 ChangedTint() { return ImGui::GetColorU32(ImVec4(kAmber.x, kAmber.y, kAmber.z, 0.13f)); }
// The screen x range of the settings table being drawn, for full-width tints.
inline float& TableLeft() { static float x{}; return x; }
inline float& TableRight() { static float x{}; return x; }
// Rows get a state column while a decor compares them, or while Base shows which
// settings a weather override replaces.
inline bool HasStateColumn() { return ActivePresetDecor() || ActiveOverrideNotes(); }
// Tints the current table row across the whole window, edge to edge. The table's
// own row background covers its width, under the cells; strips extend it to the
// window's edges on either side. Call right after TableNextRow.
inline void TintRowFullWidth(ImU32 colour)
{
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, colour);
    const auto& style = ImGui::GetStyle();
    const auto pos = ImGui::GetWindowPos();
    // Padding only exists in padded windows such as the pass panel; elsewhere stay
    // inside the window and clear of its scrollbar.
    const float scrollbar = ImGui::GetScrollMaxY() > 0 ? style.ScrollbarSize : 0;
    const float left = (std::max)(pos.x, pos.x + ImGui::GetWindowContentRegionMin().x - style.WindowPadding.x);
    const float right = (std::min)(pos.x + ImGui::GetWindowWidth() - scrollbar,
                                   pos.x + ImGui::GetWindowContentRegionMax().x + style.WindowPadding.x);
    const float top = ImGui::GetCursorScreenPos().y;
    const float bottom = top + ImGui::GetFrameHeight() + style.CellPadding.y * 2;
    auto* draw = ImGui::GetWindowDrawList();
    // The strips sit outside the table's columns, so widen the clip sideways only:
    // its top and bottom still follow the window, which hides rows scrolled out of view.
    const float clipTop = (std::max)(top, draw->GetClipRectMin().y);
    const float clipBottom = (std::min)(bottom, draw->GetClipRectMax().y);
    if (clipTop >= clipBottom) { return; }
    draw->PushClipRect(ImVec2(left, clipTop), ImVec2(right, clipBottom), false);
    if (TableLeft() > left) { draw->AddRectFilled(ImVec2(left, top), ImVec2(TableLeft(), bottom), colour); }
    if (TableRight() < right) { draw->AddRectFilled(ImVec2(TableRight(), top), ImVec2(right, bottom), colour); }
    draw->PopClipRect();
}

// What a row's ! mark says on hover. NR's text by default; other tabs set their own.
inline const char*& RestartHelpText()
{
    static const char* text = RestartsNRHelp;
    return text;
}
// Sets the ! text for its lifetime.
struct RestartHelpScope
{
    const char* previous;
    explicit RestartHelpScope(const char* text) : previous(RestartHelpText()) { RestartHelpText() = text; }
    ~RestartHelpScope() { RestartHelpText() = previous; }
    RestartHelpScope(const RestartHelpScope&) = delete;
    RestartHelpScope& operator=(const RestartHelpScope&) = delete;
};

namespace Rows
{
// Widths in 1x units; callers multiply by the zoom.
inline constexpr float MaxControl = 520, MinControl = 140;
// The state column holds one undo button, a frame high and square.
inline float StateColumn() { return ImGui::GetFrameHeight(); }
// One control column shared by every row table drawn while it is active, so
// controls start and end at the same x inside and outside the pass panel.
struct Grid
{
    float controlX{}, controlWidth{};
};
inline const Grid*& ActiveGrid()
{
    static const Grid* grid{};
    return grid;
}
// Screen x of a table column's content, for a table starting at tableX.
inline float ColumnStart(float tableX, float labelWidth)
{
    return tableX + labelWidth + ImGui::GetStyle().CellPadding.x * 3;
}
inline void Label(const char* label, bool changed, bool restarts)
{
    ImGui::AlignTextToFramePadding();
    if (changed) { ImGui::TextColored(kAmber, "%s", label); } else { ImGui::TextUnformatted(label); }
    if (restarts) {
        ImGui::SameLine(0, Px(5.0f));
        ImGui::TextColored(kOchre, "!");
        DrawSettingsHelp(TR(RestartHelpText()));
    }
}
}

// Label left, control right. With a decor (a preset, or Pass 2 compared with
// Pass 1) a narrow state column holds Reset on rows that differ.
inline bool BeginSettingRows(const char* id, float labelWidth)
{
    const float available = ImGui::GetContentRegionAvail().x;
    const float state = HasStateColumn() ? Rows::StateColumn() : 0;
    float control = std::clamp(available - labelWidth - state - ImGui::GetStyle().CellPadding.x * 6,
        Px(Rows::MinControl), Px(Rows::MaxControl));
    if (const auto* grid = Rows::ActiveGrid())
    {
        // The label column takes up whatever lies before the shared control column.
        const float tableX = ImGui::GetCursorScreenPos().x;
        labelWidth = (std::max)(Px(20.0f), labelWidth + grid->controlX - Rows::ColumnStart(tableX, labelWidth));
        control = grid->controlWidth;
    }
    TableLeft() = ImGui::GetCursorScreenPos().x;
    TableRight() = TableLeft() + available;
    if (!ImGui::BeginTable(id, HasStateColumn() ? 3 : 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) { return false; }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, control);
    if (HasStateColumn()) { ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch); }
    return true;
}
// Whether any of keys is changed by the preset being edited.
inline bool AnyChanged(std::initializer_list<const char*> keys)
{
    const auto* decor = ActivePresetDecor();
    return decor && std::ranges::any_of(keys, [&](const char* key) { return key && decor->Changed(key); });
}
// A square button with an undo arrow, a frame high. Returns true when clicked.
inline bool UndoButton(const char* id)
{
    const float size = ImGui::GetFrameHeight();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    const auto min = ImGui::GetItemRectMin();
    auto* draw = ImGui::GetWindowDrawList();
    const auto& style = ImGui::GetStyle();
    draw->AddRectFilled(min, ImVec2(min.x + size, min.y + size),
        ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button), style.FrameRounding);
    draw->AddRect(min, ImVec2(min.x + size, min.y + size), ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);
    // The arrow in 16-unit space: a hook running right, round and back, with its head at the top left.
    const float icon = size * 0.56f, unit = icon / 16.0f;
    const ImVec2 origin(min.x + (size - icon) * 0.5f, min.y + (size - icon) * 0.5f);
    const auto at = [&](float x, float y) { return ImVec2(origin.x + x * unit, origin.y + y * unit); };
    const auto colour = ImGui::GetColorU32(hovered ? kIvory : kAmber);
    const float stroke = (std::max)(1.0f, Px(1.6f));
    draw->PathLineTo(at(5.2f, 4.2f));
    draw->PathLineTo(at(10.0f, 4.2f));
    draw->PathArcTo(at(10.0f, 7.8f), 3.6f * unit, -1.5707964f, 1.5707964f);
    draw->PathLineTo(at(5.0f, 11.4f));
    draw->PathStroke(colour, 0, stroke);
    draw->PathLineTo(at(7.4f, 1.6f));
    draw->PathLineTo(at(4.6f, 4.2f));
    draw->PathLineTo(at(7.4f, 6.8f));
    draw->PathStroke(colour, 0, stroke);
    return clicked;
}
// NR styles by name, as OptiScaler labels 0 to 2; others are custom numbers.
inline std::string NeuralStyleName(int style)
{
    switch (style)
    {
    case 0: return TR("Default");
    case 1: return TR("Natural");
    case 2: return TR("Cinematic");
    default: return TRF("Custom {}", style);
    }
}
// NR network presets: 0 is 310.9.16's small FP8 model, 1 the older model 310.8 also runs.
inline const char* NeuralNetworkName(int preset) { return preset != 0 ? TR("Preset 1") : TR("Runtime default"); }
inline std::string NeuralSubmittedNetwork(const TheosRenderPipeline::SourceDLSSG::NeuralSnapshot& state, int pass)
{
    const int submitted = state.submittedPresets[static_cast<std::size_t>(pass)];
    if (!state.active || submitted < 0) { return TR("Not submitted"); }
    using namespace TheosRenderPipeline::NeuralRendering;
    return std::format("{}: {} | {} | {}", TR("Preset"), submitted, RuntimeName(state.runtimeBuild),
        state.runtimeBuild == RuntimeBuild::Nexus3108 ? TR("Old") : TR("Model identity unverified"));
}

// One option of a segmented control. A disabled option shows greyed, with
// reason as its tooltip.
struct Segment
{
    const char* label;
    bool enabled{true};
    const char* reason{nullptr};
    // Green text when not selected, as the current time of day is marked.
    bool now{false};
};
// Where a segmented control was drawn this frame, for controls drawn over a segment.
struct SegmentedLayout
{
    int clicked{-1};      // The segment clicked this frame, selected or not.
    bool combo{false};    // Fell back to a combo; no segment rectangles.
    ImVec2 lastMin{}, lastMax{};
};
// A row of joined buttons for a short, fixed set of options; the selected one is
// amber. Fills the next item width. When the labels do not fit it falls back to
// a combo with the same options. Returns true when the selection changes.
inline bool Segmented(const char* id, int& selected, std::initializer_list<Segment> segments, SegmentedLayout* layout = nullptr)
{
    const auto& style = ImGui::GetStyle();
    const float width = ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    const auto count = static_cast<int>(segments.size());
    float needed = 0;
    for (const auto& segment : segments) { needed += ImGui::CalcTextSize(segment.label).x + style.FramePadding.x * 2; }
    bool changed = false;
    ImGui::PushID(id);
    if (needed > width || count == 0)
    {
        // Too narrow: the same choice as a combo.
        const auto* current = selected >= 0 && selected < count ? (segments.begin() + selected)->label : "";
        ImGui::SetNextItemWidth(width);
        if (layout)
        {
            // The combo stands in for every segment, the last one included.
            layout->combo = true;
            layout->lastMin = ImGui::GetCursorScreenPos();
            layout->lastMax = ImVec2(layout->lastMin.x + width, layout->lastMin.y + height);
        }
        if (ImGui::BeginCombo("##segments", current))
        {
            int index = 0;
            for (const auto& segment : segments)
            {
                ImGui::BeginDisabled(!segment.enabled);
                if (ImGui::Selectable(segment.label, index == selected))
                {
                    if (layout) { layout->clicked = index; }
                    if (index != selected) { selected = index; changed = true; }
                }
                ImGui::EndDisabled();
                if (segment.reason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", segment.reason); }
                ++index;
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }
    // Widths share the spare room equally, so short labels do not look cramped.
    const float spare = (width - needed) / count;
    auto* draw = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetCursorScreenPos();
    const auto border = ImGui::GetColorU32(ImGuiCol_Border);
    float x = origin.x;
    int index = 0;
    for (const auto& segment : segments)
    {
        const float w = index + 1 == count ? origin.x + width - x
                                           : std::round(ImGui::CalcTextSize(segment.label).x + style.FramePadding.x * 2 + spare);
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        ImGui::PushID(index);
        ImGui::BeginDisabled(!segment.enabled);
        // A control drawn over a segment later, such as an edit field, takes its clicks.
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##segment", ImVec2(w, height)))
        {
            if (layout) { layout->clicked = index; }
            if (index != selected) { selected = index; changed = true; }
        }
        if (layout && index + 1 == count) { layout->lastMin = ImVec2(x, origin.y); layout->lastMax = ImVec2(x + w, origin.y + height); }
        ImGui::EndDisabled();
        const bool hovered = ImGui::IsItemHovered();
        if (segment.reason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", segment.reason); }
        const bool on = index == selected;
        // Unavailable options sit darker than the rest, so they read as out of reach.
        const auto fill = on ? ImGui::GetColorU32(ImVec4(0.23f, 0.18f, 0.08f, 1.0f))
                        : !segment.enabled ? ImGui::GetColorU32(ImVec4(0.07f, 0.075f, 0.078f, 1.0f))
                        : hovered ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered) : ImGui::GetColorU32(ImGuiCol_FrameBg);
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + w, origin.y + height), fill);
        if (index > 0) { draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), border); }
        const auto size = ImGui::CalcTextSize(segment.label);
        // A disabled row already dims everything drawn; a disabled option uses the disabled text colour.
        const auto colour = !segment.enabled ? ImGui::GetColorU32(ImVec4(kMuted.x * 0.7f, kMuted.y * 0.7f, kMuted.z * 0.7f, 1.0f))
                          : on ? ImGui::GetColorU32(kAmber) : segment.now ? ImGui::GetColorU32(kSage)
                          : ImGui::GetColorU32(ImGuiCol_Text);
        draw->AddText(ImVec2(x + (w - size.x) * 0.5f, origin.y + (height - size.y) * 0.5f), colour, segment.label);
        ImGui::PopID();
        x += w;
        ++index;
    }
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), border, style.FrameRounding);
    // Register the whole control as one item for layout and SameLine.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
    return changed;
}

// An item someone added to a list, such as an exact weather: an amber chip that
// is removed by clicking it. A muted chip has greyed text. Returns true when
// clicked. Choices that can be on together use checkboxes; pick-one choices use
// joined segments.
inline bool Chip(const char* label, bool muted = false)
{
    const auto& style = ImGui::GetStyle();
    const auto text = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 size(text.x + style.FramePadding.x * 2, ImGui::GetFrameHeight());
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const auto topLeft = ImGui::GetItemRectMin();
    const ImVec2 bottomRight(topLeft.x + size.x, topLeft.y + size.y);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(topLeft, bottomRight, ImGui::GetColorU32(ImVec4(0.23f, 0.18f, 0.08f, 1.0f)), style.FrameRounding);
    // The border brightens on hover, since a click removes the chip.
    draw->AddRect(topLeft, bottomRight, ImGui::GetColorU32(hovered ? kAmber : kAmberDim), style.FrameRounding);
    const auto colour = ImGui::GetColorU32(muted ? kMuted : kAmber);
    const char* end = label;
    while (*end && !(end[0] == '#' && end[1] == '#')) { ++end; }
    draw->AddText(ImVec2(topLeft.x + style.FramePadding.x, topLeft.y + (size.y - text.y) * 0.5f), colour, label, end);
    return clicked;
}
// A row label with help; a dotted underline shows it has a tooltip.
inline void HintLabel(const char* label, const char* help)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    const auto topLeft = ImGui::GetItemRectMin(), bottomRight = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const float dot = (std::max)(1.0f, std::round(Px(1.0f))), y = std::round(bottomRight.y);
    for (float x = topLeft.x; x + dot <= bottomRight.x; x += dot * 3) {
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + dot, y + dot), ImGui::GetColorU32(kMuted));
    }
    DrawSettingsHelp(help);
}
// A cloud outline filling a square of size, marking a setting a weather override replaces.
inline void DrawCloud(ImDrawList* draw, ImVec2 min, float size, ImU32 colour)
{
    // Three lobes on a flat base in 16-unit space; each arc ends where its circle meets the next.
    const float unit = size / 16.0f;
    const ImVec2 origin(min.x, min.y + (size - 8.8f * unit) * 0.5f - 2.2f * unit);
    const auto at = [&](float x, float y) { return ImVec2(origin.x + x * unit, origin.y + y * unit); };
    draw->PathLineTo(at(4.0f, 11.0f));
    draw->PathLineTo(at(12.2f, 11.0f));
    draw->PathArcTo(at(12.2f, 7.7f), 3.3f * unit, 1.5707964f, -1.8371034f);
    draw->PathArcTo(at(7.9f, 5.9f), 3.7f * unit, -0.3832812f, -3.1166692f);
    draw->PathArcTo(at(4.0f, 8.4f), 2.6f * unit, -1.4933539f, -4.7123890f);
    draw->PathStroke(colour, ImDrawFlags_Closed, (std::max)(1.0f, Px(1.3f)));
}
// The state cell for one row: an undo button when it differs from what the decor
// compares with, a cloud when a weather override replaces it now, or nothing.
inline void StateCell(std::initializer_list<const char*> keys)
{
    if (!HasStateColumn()) { return; }
    ImGui::TableNextColumn();
    auto* decor = ActivePresetDecor();
    std::string base;
    for (const auto* key : keys) {
        if (decor && key && decor->Changed(key)) { base += std::format("{}{}", base.empty() ? "" : ", ", decor->BaseValue(key)); }
    }
    if (base.empty())
    {
        const auto* notes = ActiveOverrideNotes();
        std::string note;
        for (const auto* key : keys) {
            if (!notes || !key) { continue; }
            if (auto text = notes->Note(key); !text.empty() && note.find(text) == std::string::npos) {
                note += (note.empty() ? "" : "\n") + text;
            }
        }
        if (note.empty()) { return; }
        const float size = ImGui::GetFrameHeight();
        const auto min = ImGui::GetCursorScreenPos();
        ImGui::PushID(*keys.begin());
        ImGui::Dummy(ImVec2(size, size));
        ImGui::PopID();
        const float icon = std::round(size * 0.72f);
        DrawCloud(ImGui::GetWindowDrawList(), ImVec2(min.x + (size - icon) * 0.5f, min.y + (size - icon) * 0.5f), icon,
                  ImGui::GetColorU32(kAmber));
        DrawSettingsHelp(note.c_str());
        return;
    }
    // Rows disable only their label and control, so Reset stays usable.
    ImGui::PushID(*keys.begin());
    if (UndoButton("##reset")) {
        for (const auto* key : keys) { if (key && decor->Changed(key)) { decor->Reset(key); } }
    }
    ImGui::PopID();
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip(TR("Was %s in %s. Click to reset."), base.c_str(), decor->Reference()); }
}
// One row for the settings in keys; Reset restores all of them that differ.
// disabled greys the label and control that cannot apply now; the state cell stays usable.
template <class Control>
bool SettingRowFor(const char* label, std::initializer_list<const char*> keys, bool restarts, bool disabled,
    Control&& control)
{
    const bool changed = AnyChanged(keys);
    ImGui::TableNextRow();
    if (changed) { TintRowFullWidth(ChangedTint()); }
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(disabled);
    Rows::Label(label, changed, restarts);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    const bool result = control();
    ImGui::PopID();
    ImGui::EndDisabled();
    StateCell(keys);
    return result;
}
template <class Control>
bool SettingRow(const char* label, const char* key, bool restarts, bool disabled, Control&& control)
{
    return SettingRowFor(label, {key}, restarts, disabled, std::forward<Control>(control));
}

// The Neural Rendering settings' shared column, from the current cursor: room for
// the longest label outside the pass panel, or inside it after the panel's
// padding, and always room for a Reset column on the right.
inline Rows::Grid NeuralRowsGrid()
{
    const auto& style = ImGui::GetStyle();
    const float origin = ImGui::GetCursorScreenPos().x;
    const float available = ImGui::GetContentRegionAvail().x;
    const float inset = style.WindowPadding.x;
    const float outside = LabelWidth({TR("Object-specific NR"), TR("Scene normalization"), TR("Maximum scene gain"),
                                      TR("Maximum luma ratio"), TR("Neural Rendering"), TR("Reconstruction")});
    const float inside = LabelWidth({TR("Peripheral compression"), TR("Local structure"), TR("UI correction"), TR("Skin structure")});
    Rows::Grid grid;
    grid.controlX = Rows::ColumnStart(origin, (std::max)(outside, inset + inside));
    // Controls fill the column up to the undo button, which ends at the pass
    // panel's inner edge; there is no maximum width, so no gap opens beyond it.
    const float right = origin + available - inset - Rows::StateColumn() - style.CellPadding.x * 3;
    grid.controlWidth = (std::max)(Px(Rows::MinControl), right - grid.controlX);
    return grid;
}
// Makes grid the active column for its lifetime.
struct GridScope
{
    const Rows::Grid* previous;
    explicit GridScope(const Rows::Grid& grid) : previous(Rows::ActiveGrid()) { Rows::ActiveGrid() = &grid; }
    ~GridScope() { Rows::ActiveGrid() = previous; }
    GridScope(const GridScope&) = delete;
    GridScope& operator=(const GridScope&) = delete;
};

}
