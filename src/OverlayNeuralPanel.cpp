#include "OverlayUI.h"
#include "Localization.h"
#include "OverlayUIStyle.h"
#include "OverlaySettingRows.h"
#include "OverlayFrameView.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <vector>
#include <imgui_internal.h>
#include <string_view>
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

#include "NeuralRenderingMode.h"
#include "FrameGen/NeuralObjectMask.h"
#include "FrameGen/NeuralRuntimeFiles.h"
#include "FrameGen/NeuralAdmission.h"

namespace
{
// Base only: the live, session-only NR comparison. Set while the panel draws
// Base; weather overrides never show it.
std::function<void(TheosRenderPipeline::NeuralRendering::CompareSettings)> g_setCompare;
// Base only, while the panel draws Base in Lab mode: show the Lab model-rate switch.
bool g_labModelRate = false;

void DrawNRAppliedPasses(const OverlayUI::FrameView& view, const TheosRenderPipeline::SourceDLSSG::NeuralOptions& applied)
{
    const auto width = applied.beforeUpscaling ? view.renderWidth : view.outputWidth;
    const auto height = applied.beforeUpscaling ? view.renderHeight : view.outputHeight;
    DrawSettingsValue(TR("Placement"), applied.beforeUpscaling ? TR("Before upscaling") : TR("After upscaling"));
    DrawSettingsValue(TR("Scene"), std::format("{} x {}", width, height).c_str());
    auto pass = [&](const char* label, float scale, int preset) {
        auto config = applied.reconstruction;
        config.inputScale = scale;
        ImGui::Separator();
        DrawSettingsValue(label,
                          std::format("{} x {}", TheosRenderPipeline::NeuralRendering::ModelExtent(width, config),
                                      TheosRenderPipeline::NeuralRendering::ModelExtent(height, config))
                              .c_str());
        DrawSettingsValue(TR("Network"), NeuralNetworkName(preset));
    };
    pass(TR("Pass 1"), applied.reconstruction.inputScale, applied.reconstruction.preset);
    for (int index = 2; index <= applied.passes; ++index)
    {
        const auto extra = applied.EffectiveExtra(static_cast<std::size_t>(index - 2));
        pass(TRF("Pass {}", index).c_str(), extra.inputScale, extra.preset);
        DrawSettingsValue(TR("Settings"), extra.linked ? TR("Linked") : TR("Independent"));
    }
}

// Why NR cannot run now, or null. Shown above Base's controls.
const char* NeuralUnavailableReason(const OverlayUI::FrameView& view, int upscaleType, bool nrRuntimePresent)
{
    if (!view.neuralAdmissionReason.empty()) { return TR(view.neuralAdmissionReason.c_str()); }
    if (view.neuralState.sessionDisabled && !view.neuralState.reason.empty()) { return view.neuralState.reason.c_str(); }
    if (view.sourceReady && !view.nvidiaAdapter) { return TR("Neural Rendering requires NVIDIA RTX hardware. XeSS upscaling and XeFG remain available."); }
    if (view.neuralRuntimePath.empty())
    {
        return TR("NR runtime path is empty. Configure NeuralRenderingRuntimePath in TheosRenderPipeline.ini "
                  "and restart Skyrim.");
    }
    if (!nrRuntimePresent)
    {
        return TR("NR runtime DLL not found. Install the optional nvngx_dlssnr.dll at the path below and restart Skyrim.");
    }
    if (!view.sourceReady)
    {
        return TR("The NVIDIA host is not ready. Check runtime status and restart Skyrim.");
    }
    if (view.neuralState.failed && !view.neuralState.retryable)
    {
        return TR("NR failed. See the error in the left column.");
    }
    if (!TheosRenderPipeline::SupportsNeuralRenderingMode(upscaleType, view.communityShaders))
    {
        return TR("NR requires DLSS or DLAA mode. Select either in Image and restart Skyrim.");
    }
    if (!view.communityShaders && !view.dedicatedUITexture)
    {
        return TR("NR requires dedicated UI composition. Set NativeUICompositionMode=0 in the INI and "
                  "restart Skyrim. If it is already 0, check the log for a composition failure.");
    }
    return nullptr;
}

// Whether the NR runtime in use predates the reconstruction contract, which
// object tone and structure need. Unknown until NR has applied a runtime.
bool LegacyNeuralRuntime(const OverlayUI::FrameView& view)
{
    const auto& applied = view.neuralApplied;
    if (!applied.enabled || applied.runtimePath.empty()) { return false; }
    const auto name = TheosRenderPipeline::SourceDLSSG::FoldRuntimePath(applied.runtimePath.filename().string());
    return name == "nvngx_dlssnr.dll" || name.starts_with("nvngx_dlssnr_int8");
}

// An open, amber eye when shown; a dim, slashed eye when hidden. Fits a square of size.
void DrawEye(ImDrawList* draw, ImVec2 min, float size, bool shown, bool hovered)
{
    const float w = size, h = size * 0.62f;
    const ImVec2 left(min.x, min.y + size * 0.5f), right(min.x + w, min.y + size * 0.5f);
    const ImVec2 centre(min.x + w * 0.5f, min.y + size * 0.5f);
    const auto colour = ImGui::GetColorU32(shown ? kAmber : hovered ? kIvory : kMuted);
    const float stroke = (std::max)(1.0f, Px(1.4f));
    draw->AddBezierQuadratic(left, ImVec2(centre.x, centre.y - h), right, colour, stroke);
    draw->AddBezierQuadratic(left, ImVec2(centre.x, centre.y + h), right, colour, stroke);
    if (shown)
    {
        draw->AddCircleFilled(centre, size * 0.17f, colour);
    }
    else
    {
        draw->AddCircle(centre, size * 0.15f, colour, 0, stroke);
        draw->AddLine(ImVec2(min.x + w * 0.15f, min.y + size * 0.88f), ImVec2(min.x + w * 0.85f, min.y + size * 0.12f),
                      colour, stroke);
    }
}

// The groups shown in the preview, for its footer: "NPCs and Creatures".
std::string ShownGroups(unsigned groups, const char* const (&names)[4])
{
    std::vector<const char*> shown;
    for (unsigned i = 0; i < 4; ++i) { if ((groups >> i) & 1) { shown.push_back(TR(names[i])); } }
    if (shown.size() == 4) { return TR("all groups"); }
    std::string text;
    for (std::size_t i = 0; i < shown.size(); ++i)
    {
        text += i == 0 ? "" : i + 1 == shown.size() ? " and " : ", ";
        text += shown[i];
    }
    return text;
}

// One pass's object rules: the mask preview, then a row per group with its
// override checkbox and Strength, Tone and Structure. Aligned with the shared
// control column. Presets cannot change object rules, so they show greyed.
void DrawPassObjects(const OverlayUI::FrameView& view, TheosRenderPipeline::NeuralRendering::ObjectMaskSettings& masks,
                     unsigned pass, bool unavailable, bool preset)
{
    namespace NR = TheosRenderPipeline::NeuralRendering;
    static constexpr const char* names[]{"First person", "Player", "NPCs", "Creatures"};  // translated where shown
    static const ImVec4 swatches[]{{0.37f, 0.72f, 0.65f, 1}, {0.44f, 0.62f, 0.88f, 1}, {0.85f, 0.65f, 0.28f, 1},
                                   {0.70f, 0.54f, 0.85f, 1}};
    const auto& style = ImGui::GetStyle();
    const auto* grid = Rows::ActiveGrid();
    const float tableX = ImGui::GetCursorScreenPos().x;
    const float label = grid ? (std::max)(Px(40.0f), grid->controlX - Rows::ColumnStart(tableX, 0))
                             : LabelWidth({TR("Peripheral compression")});
    const float control = grid ? grid->controlWidth : Px(Rows::MaxControl);
    const float check = ImGui::GetFrameHeight();
    const float slider = (std::max)(Px(30.0f), (control - check - style.ItemSpacing.x * 3) / 3);
    const bool legacy = LegacyNeuralRuntime(view);
    const bool locked = unavailable || preset || !view.neuralObjectsAvailable;
    const char* lockedReason = preset ? TR("Object rules are set in Base; weather overrides do not change them.")
                             : !view.neuralObjectsAvailable ? view.neuralObjectStatus.c_str()
                                                            : TR("Neural Rendering is unavailable.");
    auto& rules = masks.passes[pass];
    if (!ImGui::BeginTable("passObjects", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) { return; }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, control);

    // Heading: the column names sit over the sliders below.
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Objects"));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", TR("experimental"));
    DrawSettingsHelp(locked ? lockedReason
        : TR("Tick a group to override it in this pass. Strength blends this pass's result with its input: 0 keeps "
          "the input, 1 the full result, so Pass 2 at 0 keeps Pass 1's work. Tone and structure replace this "
          "pass's values for the group and pause its automatic skin mask. Rules are independent of the other passes "
          "and of weather overrides. Inference still runs at any strength."));
    ImGui::TableNextColumn();
    const float left = ImGui::GetCursorPosX() + check + style.ItemSpacing.x;
    int column = 0;
    for (const char* heading : {"Strength", "Tone", "Structure"})
    {
        const float x = left + column++ * (slider + style.ItemSpacing.x);
        if (column > 1) { ImGui::SameLine(); }
        ImGui::SetCursorPosX(x + (slider - ImGui::CalcTextSize(heading).x) * 0.5f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", heading);
    }

    ImGui::BeginDisabled(locked);
    for (unsigned i = 0; i < 4; ++i)
    {
        auto& rule = rules.groups[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // The eye shows this group in the mask preview, whether or not it overrides.
        // Eye, swatch and name share one frame-height line, like the controls.
        const float frame = ImGui::GetFrameHeight();
        const float eye = std::round(ImGui::GetTextLineHeight());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(14.0f));
        bool shown = (masks.previewGroups >> i) & 1;
        if (ImGui::InvisibleButton("##eye", ImVec2(eye, frame)))
        {
            shown = !shown;
            masks.previewGroups = shown ? (masks.previewGroups | (1u << i)) : (masks.previewGroups & ~(1u << i));
        }
        const bool eyeHovered = ImGui::IsItemHovered();
        if (eyeHovered)
        {
            ImGui::SetTooltip(shown ? TR("Hide %s in the mask preview.") : TR("Show %s in the mask preview."), TR(names[i]));
        }
        const auto eyeMin = ImGui::GetItemRectMin();
        DrawEye(ImGui::GetWindowDrawList(), ImVec2(eyeMin.x, eyeMin.y + (frame - eye) * 0.5f), eye, shown, eyeHovered);
        ImGui::SameLine(0, style.ItemInnerSpacing.x * 1.5f);
        const float box = std::round(Px(8.0f));
        ImGui::Dummy(ImVec2(box, frame));
        const auto swatch = ImGui::GetItemRectMin();
        const float top = swatch.y + (frame - box) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(swatch.x, top), ImVec2(swatch.x + box, top + box),
            ImGui::GetColorU32(swatches[i]), Px(2.0f));
        ImGui::SameLine(0, style.ItemInnerSpacing.x);
        ImGui::AlignTextToFramePadding();
        // An overriding group's name is amber, as a changed setting is.
        if (rule.enabled) { ImGui::TextColored(kAmber, "%s", TR(names[i])); } else { ImGui::TextUnformatted(TR(names[i])); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", TR(NR::kObjectGroups[i])); }
        ImGui::TableNextColumn();
        ImGui::Checkbox("##override", &rule.enabled);
        DrawSettingsHelp(rule.enabled ? TR("Overriding this group in this pass.") : TR("Override this group in this pass."));
        ImGui::BeginDisabled(!rule.enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(slider);
        ImGui::SliderFloat("##strength", &rule.strength, 0, 1, "%.2f");
        DrawSettingsHelp(TR("Strength: 0 keeps this pass's input, 1 its full result."));
        ImGui::BeginDisabled(legacy);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(slider);
        ImGui::SliderFloat("##tone", &rule.tone, 0, 1, "%.2f");
        DrawSettingsHelp(legacy ? TR("Tone needs the 310.9.16 runtime; Strength still applies.") : TR("Local tone for this group in this pass."));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##structure", &rule.structure, 0, 1, "%.2f");
        DrawSettingsHelp(legacy ? TR("Structure needs the 310.9.16 runtime; Strength still applies.") : TR("Local structure for this group in this pass."));
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(14.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", TR("Mask preview"));
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!masks.previewGroups);
    ImGui::Checkbox(TR("Classes only"), &masks.previewClassesOnly);
    ImGui::EndDisabled();
    DrawSettingsHelp(masks.previewGroups ? TR("Shows only the chosen groups' colours on black.")
                                         : TR("Open a group's eye to start the preview."));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    const auto shownText = masks.previewGroups ? TRF("showing {} | not saved", ShownGroups(masks.previewGroups, names))
                                               : std::string(TR("open an eye to preview a group | not saved"));
    ImGui::TextDisabled("%s", shownText.c_str());
    DrawSettingsHelp(TR("The preview covers the whole finished image, so it is the same on every pass tab."));
    ImGui::EndDisabled();
    ImGui::EndTable();

    // What capture is doing now, when it matters.
    const bool inUse = NR::ObjectsInUse(masks, NR::kMaxNeuralPasses);
    if (!view.neuralObjectsAvailable || inUse)
    {
        ImGui::Indent(Px(14.0f));
        // The status comes from the object hooks in English; translate it where it is shown.
        ImGui::TextDisabled("%s%s", TheosRenderPipeline::Localization::TranslateText(view.neuralObjectStatus).c_str(),
            !inUse || !view.neuralObjectsAvailable ? ""
            : view.neuralObjectCurrent ? TR(" | classifying this frame") : TR(" | no classification this frame; normal NR"));
        ImGui::Unindent(Px(14.0f));
    }
}

// The pass shown under the pass tabs. Base and presets share it.
int& SelectedPass()
{
    static int selected{};
    return selected;
}

// An extra pass's settings compared with Pass 1's, for the state column.
struct PassDecor final : PresetDecor
{
    TheosRenderPipeline::NeuralRendering::ExtraPassSettings& shown;
    const TheosRenderPipeline::NeuralRendering::Reconstruction& first;
    const TheosRenderPipeline::NeuralRendering::Tuning& tuning;
    // This pass's key prefix, "Pass2" to "Pass5".
    std::string prefix;
    PassDecor(TheosRenderPipeline::NeuralRendering::ExtraPassSettings& shown,
        const TheosRenderPipeline::NeuralRendering::Reconstruction& first,
        const TheosRenderPipeline::NeuralRendering::Tuning& tuning, int pass)
        : shown(shown), first(first), tuning(tuning), prefix(std::format("Pass{}", pass)) {}

    // Each pass's own settings, after its "PassN" prefix.
    static constexpr const char* kKeys[]{"Style", "Intensity", "LocalTone", "LocalStructure",
        "SkinStructure", "AutoSkinMask", "UICorrection", "InputScale", "Network"};
    enum class Kind { Style, Value, Percent, Toggle, Network };

    // Calls visit(this pass's value, Pass 1's value, kind) for key; false for other keys.
    template <class Visit> bool With(std::string_view key, Visit&& visit) const
    {
        if (!key.starts_with(prefix)) { return false; }
        key.remove_prefix(prefix.size());
        auto& own = shown.tuning;
        if (key == "Style") { visit(own.style, tuning.style, Kind::Style); }
        else if (key == "Intensity") { visit(own.intensity, tuning.intensity, Kind::Value); }
        else if (key == "LocalTone") { visit(own.localToneStrength, tuning.localToneStrength, Kind::Value); }
        else if (key == "LocalStructure") { visit(own.localStructureStrength, tuning.localStructureStrength, Kind::Value); }
        else if (key == "SkinStructure") { visit(own.skinStructureStrength, tuning.skinStructureStrength, Kind::Value); }
        else if (key == "AutoSkinMask") { visit(own.useAutoSkinMask, tuning.useAutoSkinMask, Kind::Toggle); }
        else if (key == "UICorrection") { visit(own.uiCorrection, tuning.uiCorrection, Kind::Toggle); }
        else if (key == "InputScale") { visit(shown.inputScale, first.inputScale, Kind::Percent); }
        else if (key == "Network") { visit(shown.preset, first.preset, Kind::Network); }
        else { return false; }
        return true;
    }
    bool Changed(const char* key) const override
    {
        bool changed = false;
        With(key, [&](auto& own, const auto& reference, Kind) { changed = own != reference; });
        return changed;
    }
    std::string BaseValue(const char* key) const override
    {
        std::string text;
        With(key, [&](auto&, const auto& reference, Kind kind) {
            const auto value = static_cast<double>(reference);
            switch (kind)
            {
            case Kind::Style: text = NeuralStyleName(static_cast<int>(value)); break;
            case Kind::Percent: text = std::format("{:.1f}%", value * 100); break;
            case Kind::Toggle: text = value != 0 ? TR("On") : TR("Off"); break;
            case Kind::Network: text = NeuralNetworkName(value != 0 ? 1 : 0); break;
            default: text = std::format("{:.3f}", value); break;
            }
        });
        return text;
    }
    void Reset(const char* key) override
    {
        With(key, [](auto& own, const auto& reference, Kind) { own = reference; });
    }
    const char* EditingTime() const override { return nullptr; }
    const char* Reference() const override { return TR("Pass 1"); }
    int Differences() const
    {
        return static_cast<int>(std::ranges::count_if(kKeys, [&](const char* key) { return Changed((prefix + key).c_str()); }));
    }
};

enum class PassAction { None, Add, Remove, FollowPass1 };

// The selected pass's folder tab joins the panel below; the others sit behind.
// Its background matches the panel's.
const ImVec4 kPassPanel{0.098f, 0.104f, 0.106f, 1.0f};
const ImVec4 kPassBehind{0.058f, 0.063f, 0.066f, 1.0f};

struct PassPanelGeometry
{
    float tabLeft{}, tabRight{};
    // The reset tab at the strip's right end, open into the panel too; 0 width when absent.
    float resetLeft{}, resetRight{};
};
PassPanelGeometry g_passGeometry;

void FillRect(ImDrawList* draw, float x0, float y0, float x1, float y1, ImU32 color)
{
    draw->AddRectFilled(ImVec2(std::floor(x0), std::floor(y0)), ImVec2(std::floor(x1), std::floor(y1)), color);
}

// Pass tabs, then + to add the next pass. note is drawn at the right, muted.
// changed[pass] tints a tab's name amber: it differs from Base in a preset, or
// an extra pass differs from Pass 1.
// How the strip's right end offers "Reset to Pass 1" for the selected extra
// pass: not at all (Pass 1), greyed while it follows Pass 1, or usable.
enum class PassReset { None, Nothing, Available };

PassAction DrawPassTabs(const OverlayUI::FrameView& view, int& selected, int passes, bool editable, const std::string& note,
    const bool (&changed)[TheosRenderPipeline::NeuralRendering::kMaxNeuralPasses], PassReset reset)
{
    constexpr int kMaxPasses = TheosRenderPipeline::NeuralRendering::kMaxNeuralPasses;
    auto* draw = ImGui::GetWindowDrawList();
    const auto& style = ImGui::GetStyle();
    const auto origin = ImGui::GetCursorScreenPos();
    const float available = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight() + Px(6.0f);
    const float lift = Px(4.0f);
    const float addWidth = ImGui::GetFrameHeight() + Px(4.0f);
    const float line = std::round((std::max)(1.0f, Px(1.0f)));
    const auto border = ImGui::GetColorU32(ImGuiCol_Border);
    const auto& live = view.sourceNeural;
    const char* resetLabel = TR("Reset to Pass 1");
    const float resetWidth = reset != PassReset::None ? ImGui::CalcTextSize(resetLabel).x + style.FramePadding.x * 3 : 0.0f;
    // Tabs narrow from their usual width so five, + and the reset tab share the
    // strip; never below the name and its state dot.
    const float dot = ImGui::GetTextLineHeight() * 0.18f;
    const float tabMinimum = ImGui::CalcTextSize(TR("Pass 5")).x + dot * 2 + style.ItemInnerSpacing.x + style.FramePadding.x * 2;
    const float tabSpace = available - (passes < kMaxPasses ? addWidth : 0.0f) - (resetWidth > 0 ? resetWidth + style.ItemSpacing.x : 0.0f);
    const float tabWidth = std::floor(std::clamp((tabSpace + line * (passes - 1)) / passes, tabMinimum, Px(132.0f)));
    PassAction action = PassAction::None;

    ImGui::PushID("passTabs");
    g_passGeometry = {};
    // Register the strip's space first; the tabs are drawn over it.
    ImGui::Dummy(ImVec2(available, height));
    float x = origin.x;
    for (int pass = 0; pass < passes; ++pass)
    {
        const bool active = pass == selected;
        const float top = origin.y + (active ? 0.0f : lift);
        const float bottom = origin.y + height;
        ImGui::SetCursorScreenPos(ImVec2(x, top));
        ImGui::PushID(pass);
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("tab", ImVec2(tabWidth, bottom - top))) { selected = pass; }
        const bool hovered = ImGui::IsItemHovered();
        // Live state: whether this pass is running now.
        const char* badge = nullptr;
        ImVec4 badgeColor = kSage;
        const bool running = pass < (std::max)(1, live.effectivePasses);
        if (live.active && pass < view.neuralApplied.passes)
        {
            badge = running ? "ON" : "PAUSED";
            if (!running) { badgeColor = kOchre; }
        }
        if (hovered)
        {
            // The tooltip also names the state, which a narrow tab shows only as a dot.
            const auto state = badge ? TRF(" Now {}.", running ? TR("running") : TR("paused")) : std::string();
            if (pass == 0) { ImGui::SetTooltip("%s%s", TR("Runs on the scene."), state.c_str()); }
            else
            {
                ImGui::SetTooltip("%s%s", TRF("Processes Pass {}'s result with its own history. Adds GPU time and memory; "
                                              "weather overrides switch it on and off without restarting NR.", pass).c_str(),
                                  state.c_str());
            }
        }
        FillRect(draw, x, top, x + tabWidth, bottom + (active ? line : 0.0f),
                 ImGui::GetColorU32(active ? kPassPanel : hovered ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered) : kPassBehind));
        FillRect(draw, x, top, x + line, bottom, border);
        FillRect(draw, x, top, x + tabWidth, top + line, border);
        FillRect(draw, x + tabWidth - line, top, x + tabWidth, bottom, border);
        if (active) { FillRect(draw, x, top, x + tabWidth, top + Px(2.0f), ImGui::GetColorU32(kAmber)); }
        const auto name = TRF("Pass {}", pass + 1);
        const float textY = top + ((bottom - top) - ImGui::GetTextLineHeight()) * 0.5f;
        const auto nameColor = changed[pass] ? (active ? kAmber : kAmberDim) : active ? kIvory : kMuted;
        draw->AddText(ImVec2(x + style.FramePadding.x, textY), ImGui::GetColorU32(nameColor), name.c_str());

        float right = x + tabWidth - style.FramePadding.x;
        if (pass > 0 && pass == passes - 1 && editable && (active || hovered))
        {
            // Remove the last pass. Its own settings are kept for when it is added again.
            const float size = ImGui::GetTextLineHeight();
            ImGui::SetCursorScreenPos(ImVec2(right - size, textY));
            if (ImGui::InvisibleButton("remove", ImVec2(size, size))) { action = PassAction::Remove; }
            const bool over = ImGui::IsItemHovered();
            if (over) { ImGui::SetTooltip("%s", TRF("Remove Pass {}. Its own settings are kept for later.", pass + 1).c_str()); }
            const float inset = size * 0.28f;
            const auto color = ImGui::GetColorU32(over ? kAmber : kMuted);
            draw->AddLine(ImVec2(right - size + inset, textY + inset), ImVec2(right - inset, textY + size - inset), color, Px(1.5f));
            draw->AddLine(ImVec2(right - inset, textY + inset), ImVec2(right - size + inset, textY + size - inset), color, Px(1.5f));
            right -= size + style.ItemInnerSpacing.x;
        }
        if (badge)
        {
            const float badgeWidth = ImGui::CalcTextSize(badge).x;
            const float nameRight = x + style.FramePadding.x + ImGui::CalcTextSize(name.c_str()).x + style.ItemInnerSpacing.x;
            if (right - badgeWidth >= nameRight)
            {
                draw->AddText(ImVec2(right - badgeWidth, textY), ImGui::GetColorU32(badgeColor), badge);
            }
            else if (right - dot * 2 >= nameRight)
            {
                // Too narrow for the word: a dot in the same colour. Beside the
                // remove mark there may be no room at all; the tooltip names the state.
                draw->AddCircleFilled(ImVec2(right - dot, textY + ImGui::GetTextLineHeight() * 0.5f), dot,
                                      ImGui::GetColorU32(badgeColor));
            }
        }
        if (active) { g_passGeometry.tabLeft = x; g_passGeometry.tabRight = x + tabWidth; }
        ImGui::PopID();
        x += tabWidth - line;
    }
    if (passes < kMaxPasses)
    {
        const float top = origin.y + lift, bottom = origin.y + height;
        ImGui::SetCursorScreenPos(ImVec2(x, top));
        ImGui::BeginDisabled(!editable);
        if (ImGui::InvisibleButton("add", ImVec2(addWidth, bottom - top))) { action = PassAction::Add; }
        ImGui::EndDisabled();
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (hovered) { ImGui::SetTooltip("%s", TRF("Add Pass {}. It starts out following Pass 1.", passes + 1).c_str()); }
        FillRect(draw, x, top, x + addWidth, bottom,
                 ImGui::GetColorU32(hovered && editable ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered) : kPassBehind));
        FillRect(draw, x, top, x + line, bottom, border);
        FillRect(draw, x, top, x + addWidth, top + line, border);
        FillRect(draw, x + addWidth - line, top, x + addWidth, bottom, border);
        const auto plus = ImGui::CalcTextSize("+");
        draw->AddText(ImVec2(x + (addWidth - plus.x) * 0.5f, top + ((bottom - top) - plus.y) * 0.5f),
                      ImGui::GetColorU32(editable ? (hovered ? kAmber : kIvory) : kMuted), "+");
    }
    float right = origin.x + available;
    if (reset != PassReset::None)
    {
        // A tab on the page's own background at the strip's right end, open into
        // the panel like the selected tab, so it reads as part of that pass's page.
        // It relinks only the selected pass.
        const char* label = resetLabel;
        const float width = resetWidth;
        const float left = right - width;
        const float top = origin.y + lift, bottom = origin.y + height;
        const bool usable = editable && reset == PassReset::Available;
        ImGui::SetCursorScreenPos(ImVec2(left, top));
        ImGui::BeginDisabled(!usable);
        if (ImGui::InvisibleButton("reset", ImVec2(width, bottom - top))) { action = PassAction::FollowPass1; }
        ImGui::EndDisabled();
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (hovered)
        {
            if (usable)
            {
                ImGui::SetTooltip("%s", TRF("Pass {} follows Pass 1 again. Its own settings are kept for later. Other passes "
                                            "are unchanged.", selected + 1).c_str());
            }
            else { ImGui::SetTooltip("%s", TRF("Pass {} already follows Pass 1; nothing to reset.", selected + 1).c_str()); }
        }
        FillRect(draw, left, top, right, bottom + line, ImGui::GetColorU32(kPassPanel));
        FillRect(draw, left, top, left + line, bottom, border);
        FillRect(draw, left, top, right, top + line, border);
        FillRect(draw, right - line, top, right, bottom, border);
        const auto size = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(left + (width - size.x) * 0.5f, top + ((bottom - top) - size.y) * 0.5f),
                      ImGui::GetColorU32(!usable ? kMuted : hovered ? kAmber : kIvory), label);
        g_passGeometry.resetLeft = left;
        g_passGeometry.resetRight = right;
        right = left - style.ItemSpacing.x;
    }
    if (!note.empty())
    {
        const auto size = ImGui::CalcTextSize(note.c_str());
        draw->AddText(ImVec2(right - size.x - style.FramePadding.x, origin.y + height - size.y - style.FramePadding.y),
                      ImGui::GetColorU32(kMuted), note.c_str());
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height));
    return action;
}

// The selected pass's settings, in a panel that the selected tab opens into.
struct PassPanel
{
    PassPanelGeometry tab;
};
PassPanel BeginPassPanel()
{
    PassPanel panel{g_passGeometry};
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPassPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::BeginChild("passPanel", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    return panel;
}
void EndPassPanel(const PassPanel& panel)
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    // The border, open under the selected tab.
    auto* draw = ImGui::GetWindowDrawList();
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float line = std::round((std::max)(1.0f, Px(1.0f)));
    const auto border = ImGui::GetColorU32(ImGuiCol_Border);
    FillRect(draw, min.x, min.y, min.x + line, max.y, border);
    FillRect(draw, max.x - line, min.y, max.x, max.y, border);
    FillRect(draw, min.x, max.y - line, max.x, max.y, border);
    FillRect(draw, min.x, min.y, (std::max)(min.x, panel.tab.tabLeft + line), min.y + line, border);
    // Open under the reset tab too, when there is one.
    const float end = panel.tab.resetRight > panel.tab.resetLeft ? panel.tab.resetLeft + line : max.x;
    FillRect(draw, (std::min)(max.x, panel.tab.tabRight - line), min.y, (std::min)(max.x, end), min.y + line, border);
}

bool DrawNeuralRuntimeCombo(const OverlayUI::FrameView& view, TheosRenderPipeline::SourceDLSSG::Preferences& draft,
                            float width);

// Base and presets share these controls. Presets add each row's state column.
void DrawNeuralSettings(const OverlayUI::FrameView& view, TheosRenderPipeline::SourceDLSSG::Preferences& draft,
                        bool& sharpening, float& sharpness, bool unavailable)
{
    namespace NR = TheosRenderPipeline::NeuralRendering;
    auto& reconstruction = draft.neuralReconstruction;
    // While NR is off, let a budget-limited request reduce its demand before
    // enabling again. Hardware/edition/driver/asset refusals keep these rows grey.
    const bool editDemand = !draft.neuralEnabled && view.neuralAdmissionReason == NR::kPascalBudgetReason;
    const bool cs = view.communityShaders;
    const bool producerColor = cs && draft.neuralBeforeUpscaling;
    const float label = LabelWidth({TR("Scene normalization")});
    const auto grid = NeuralRowsGrid();
    const GridScope gridScope(grid);
    const auto width = draft.neuralBeforeUpscaling ? view.renderWidth : view.outputWidth;
    const auto height = draft.neuralBeforeUpscaling ? view.renderHeight : view.outputHeight;


    if (BeginSettingRows("nrTop", label))
    {
        // The runtime is not a preset setting, so presets show only the switch.
        const bool base = !ActivePresetDecor();
        const bool lockSwitch = !TheosRenderPipeline::CanEditNeuralEnabled(draft.neuralEnabled, !unavailable);
        SettingRow(TR("Neural Rendering"), "NeuralRendering", false, base ? (view.neuralPascal && lockSwitch) : lockSwitch, [&] {
            ImGui::BeginDisabled(base && lockSwitch);
            bool changed = ImGui::Checkbox("##v", &draft.neuralEnabled);
            ImGui::EndDisabled();
            if (base)
            {
                const auto& style = ImGui::GetStyle();
                const float mark = ImGui::CalcTextSize("!").x + style.ItemSpacing.x;
                ImGui::SameLine();
                ImGui::BeginDisabled(view.neuralPascal && !view.neuralAdmissionReason.empty());
                changed |= DrawNeuralRuntimeCombo(view, draft, ImGui::GetContentRegionAvail().x - mark);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kOchre, "!");
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", TR("Changing the runtime restarts NR briefly.")); }
            }
            return changed;
        });
        SettingRow(TR("Placement"), "BeforeUpscaling", true, unavailable && !editDemand, [&] {
            int placement = draft.neuralBeforeUpscaling ? 0 : 1;
            const bool changed = Segmented("placement", placement, {
                {TR("Before upscaling"), true, TR("NR runs on the render-resolution scene, before DLSS or XeSS.")},
                {TR("After upscaling"), true, TR("NR runs on the output-resolution image, after upscaling.")}});
            if (changed) { draft.neuralBeforeUpscaling = placement == 0; }
            DrawSettingsHelp(TR("Every pass uses the selected side of upscaling. UI is composed afterwards."));
            return changed;
        });
        {
            // Live, unlike the other ! rows: the mark carries the image caveat.
            const RestartHelpScope caveat(TR("The model runs on alternate frames. NR's change is carried to the frames "
                                          "between using motion vectors and depth, which roughly halves NR's GPU cost. "
                                          "Expect some ghosting in fast motion and on newly revealed areas. Best "
                                          "with 2x frame generation."));
            SettingRow(TR("Model rate"), "ModelRate", true, unavailable, [&] {
                using NR::ModelRate;
                int rate = reconstruction.modelRate == ModelRate::EveryOtherFrame ? 1 : 0;
                const bool changed = Segmented("modelRate", rate, {
                    {TR("Every frame"), true, TR("NR's model runs on every frame.")},
                    {TR("Every other frame"), true, TR("The model runs on alternate frames; the frames between reuse its change, "
                                                "moved with the scene. Applies to every pass. Freeze frame runs it on "
                                                "every frame.")}});
                if (changed) { reconstruction.modelRate = rate == 1 ? ModelRate::EveryOtherFrame : ModelRate::EveryFrame; }
                DrawSettingsHelp(TR("How often NR's model runs, for every pass. Apply switches it live."));
                if (base && g_labModelRate)
                {
                    // Session-only, like the other Lab switches: never saved, off at every start.
                    const bool alternate = reconstruction.modelRate == ModelRate::EveryOtherFrame;
                    bool third = NR::LabEveryThirdFrame();
                    ImGui::BeginDisabled(!alternate);
                    if (ImGui::Checkbox(TR("Every third frame (Lab)"), &third)) { NR::LabEveryThirdFrame() = third; }
                    ImGui::EndDisabled();
                    DrawSettingsHelp(alternate
                        ? TR("Not saved. Every other frame runs the model on one frame in three instead; the two frames "
                             "between carry its change on, one frame at a time. Saves little more GPU time, with more "
                             "ghosting in fast motion and less even pacing.")
                        : TR("Applies with Every other frame. Not saved."));
                }
                // Each real frame starts its generated frames when it finishes, so the
                // alternation shows between them, more as they get closer together.
                const unsigned generated = draft.provider == TheosRenderPipeline::FrameGenerationProvider::XeFG
                                               ? draft.xefg.generatedFrames : draft.generation.generatedFrames;
                if (reconstruction.modelRate == ModelRate::EveryOtherFrame && view.frameGenerationRequested && generated > 1)
                {
                    ImGui::TextDisabled("%s", TR("Best with 2x frame generation"));
                    DrawSettingsHelp(TR("At 3x and above, the generated frames between real ones can look unevenly paced: "
                                     "frames with and without the model finish at different times."));
                }
                return changed;
            });
        }
        if (base && g_setCompare)
        {
            // Applies at once and is never saved, so it is not a draft change.
            SettingRow(TR("Compare"), nullptr, false, unavailable, [&] {
                auto compare = view.neuralApplied.compare;
                bool changed = ImGui::Checkbox(TR("Split"), &compare.split);
                DrawSettingsHelp(TR("Session only, never saved. Left of the line shows the frame without NR."));
                ImGui::SameLine();
                ImGui::BeginDisabled(!compare.split);
                // The slider takes what Freeze frame leaves of the column.
                const auto& style = ImGui::GetStyle();
                const float freezeWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                                          ImGui::CalcTextSize(TR("Freeze frame")).x;
                ImGui::SetNextItemWidth((std::max)(Px(48.0f), ImGui::GetContentRegionAvail().x - freezeWidth -
                                                                  style.ItemSpacing.x * 3));
                float percent = compare.splitPosition * 100.0f;
                if (ImGui::SliderFloat("##splitAt", &percent, 0, 100, "%.0f%%"))
                {
                    compare.splitPosition = percent / 100.0f;
                    changed = true;
                }
                ImGui::EndDisabled();
                DrawSettingsHelp(TR("Where the line sits, from the left edge."));
                // Follows the applied placement: the comparison is live, the draft is not.
                const bool before = view.neuralApplied.beforeUpscaling;
                ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
                ImGui::BeginDisabled(before);
                bool freeze = compare.freeze && !before;
                if (ImGui::Checkbox(TR("Freeze frame"), &freeze))
                {
                    compare.freeze = freeze;
                    changed = true;
                }
                ImGui::EndDisabled();
                DrawSettingsHelp(before ? TR("Freeze frame needs After upscaling: before upscaling, DLSS needs a moving frame.")
                                        : TR("Session only. NR keeps working on one held frame while the game runs on, so "
                                             "setting changes compare on identical content. Turn frame generation off for a "
                                             "steady image."));
                if (changed) { g_setCompare(compare); }
                return false;
            });
        }
        ImGui::EndTable();
    }
    auto* decor = ActivePresetDecor();
    constexpr int kMaxPasses = NR::kMaxNeuralPasses;
    auto& selected = SelectedPass();
    selected = std::clamp(selected, 0, std::clamp(draft.neuralPasses, 1, kMaxPasses) - 1);
    ImGui::Spacing();
    // Keys that turn passes on or off, or make one follow Pass 1.
    static const auto linkKeys = [] {
        std::array<std::string, kMaxPasses - 1> keys;
        for (std::size_t i = 0; i < keys.size(); ++i) { keys[i] = std::format("Pass{}SameAsPass1", i + 2); }
        return keys;
    }();
    const auto passesDiffer = [&] {
        return decor && (decor->Changed("Passes") ||
                         std::ranges::any_of(linkKeys, [&](const auto& key) { return decor->Changed(key.c_str()); }));
    };
    // Each extra pass follows Pass 1 until one of its own settings changes.
    auto effective = NR::EffectiveExtraPasses(draft.neuralExtraPasses, reconstruction, draft.neuralTuning);
    bool tabChanged[kMaxPasses]{};
    for (int pass = 0; pass < kMaxPasses; ++pass)
    {
        const auto prefix = std::format("Pass{}", pass + 1);
        if (decor)
        {
            tabChanged[pass] = std::ranges::any_of(PassDecor::kKeys, [&](const char* key) {
                return decor->Changed((prefix + key).c_str());
            });
            if (pass > 0)
            {
                // A pass the preset adds, or one it relinks or unlinks.
                tabChanged[pass] |= decor->Changed(linkKeys[pass - 1].c_str()) ||
                                    (decor->Changed("Passes") && pass >= decor->ReferencePasses());
            }
        }
        else if (pass > 0)
        {
            tabChanged[pass] = PassDecor{effective[pass - 1], reconstruction, draft.neuralTuning, pass + 1}.Differences() > 0;
        }
    }
    // The selected extra pass, index 0 for Pass 2, or -1 for Pass 1.
    const int extraIndex = selected - 1;
    // Each extra pass always shows the reset tab; it is greyed while nothing differs.
    const auto reset = extraIndex < 0 ? PassReset::None
                       : !draft.neuralExtraPasses[extraIndex].linked && (decor || tabChanged[selected]) ? PassReset::Available
                                                                                                         : PassReset::Nothing;
    const auto action = DrawPassTabs(view, selected, draft.neuralPasses, !unavailable || editDemand,
        decor && decor->EditingTime() ? TRF("editing {}", decor->EditingTime()) : std::string(), tabChanged,
        reset);
    // + adds the next pass and the remove mark takes off the last; a removed
    // pass keeps its own settings for when it is added again.
    if (action == PassAction::Add && draft.neuralPasses < kMaxPasses) { selected = draft.neuralPasses++; }
    if (action == PassAction::Remove && draft.neuralPasses > 1) { selected = --draft.neuralPasses - 1; }
    const int shownExtra = selected - 1;
    const bool extraPass = shownExtra >= 0;
    // What this tab edits: a copy of an extra pass's effective settings, written
    // back and unlinked only when one of them changes.
    auto shown = extraPass ? effective[shownExtra] : NR::ExtraPassSettings{};
    const auto before = shown;
    PassDecor passDecor{shown, reconstruction, draft.neuralTuning, selected + 1};

    // In Base, an extra pass's rows compare with Pass 1 as a preset's compare with Base.
    // A preset keeps its own comparison with Base.
    const bool comparePasses = extraPass && !decor;
    if (comparePasses) { ActivePresetDecor() = &passDecor; }
    const auto panel = BeginPassPanel();
    if (passesDiffer())
    {
        // A preset that turns passes on or off, or makes one follow Pass 1.
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kAmber, TR("Passes differ from %s"), decor->Reference());
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("Reset##passes")))
        {
            if (decor->Changed("Passes")) { decor->Reset("Passes"); }
            for (const auto& key : linkKeys) { if (decor->Changed(key.c_str())) { decor->Reset(key.c_str()); } }
        }
    }

    const std::string prefix = std::format("Pass{}", selected + 1);
    const auto key = [&](const char* name) { return prefix + name; };
    auto& tuning = extraPass ? shown.tuning : draft.neuralTuning;
    float& scale = extraPass ? shown.inputScale : reconstruction.inputScale;
    int& network = extraPass ? shown.preset : reconstruction.preset;
    const float passLabel = label;
    if (BeginSettingRows("passLook", passLabel))
    {
        SettingRow(TR("Style"), key("Style").c_str(), false, unavailable, [&] {
            // Named styles as in OptiScaler forks. Custom keeps any other number for
            // users and for runtimes that may add styles: clicking it edits the
            // number in place, and it then reads "Custom 7".
            const bool legacy = LegacyNeuralRuntime(view);
            // One editor per override and pass, so a number being typed cannot land in another.
            ImGui::PushID(static_cast<int>(ActiveEditScope()));
            ImGui::PushID(selected + 1);
            auto* storage = ImGui::GetStateStorage();
            const ImGuiID editId = ImGui::GetID("customEdit"), draftId = ImGui::GetID("customDraft"),
                          frameId = ImGui::GetID("customFrame");
            int editing = storage->GetInt(editId, 0); // 0 closed, 1 focus on this draw, 2 open
            // An edit left when its row stopped drawing (another tab, pass or override, or the
            // menu closed) is dropped rather than applied later.
            if (editing && storage->GetInt(frameId, -1) != ImGui::GetFrameCount() - 1) { editing = 0; }
            storage->SetInt(frameId, ImGui::GetFrameCount());
            const auto customLabel = tuning.style > 2 ? TRF("Custom {}", tuning.style) : std::string(TR("Custom"));
            int index = (std::min)(tuning.style, 3);
            SegmentedLayout layout;
            bool changed = Segmented("style", index, {
                {TR("Default"), true, TR("Style 0.")},
                {TR("Natural"), true, TR("Style 1.")},
                {TR("Cinematic"), true, TR("Style 2.")},
                {customLabel.c_str(), !legacy, legacy ? TR("310.8 runs styles 0 to 2 only.")
                    : TR("Click to type a style number, 3 to 255.")}}, &layout);
            if (changed) { tuning.style = index == 3 ? (std::max)(tuning.style, 3) : index; }
            if (layout.clicked == 3 && !legacy) {
                editing = 1;
                storage->SetInt(draftId, (std::max)(tuning.style, 3));
            }
            if (index != 3) { editing = 0; }
            if (editing) {
                // Over the Custom segment, or over the whole combo in a narrow menu.
                ImGui::SetCursorScreenPos(layout.lastMin);
                ImGui::SetNextItemWidth(layout.lastMax.x - layout.lastMin.x);
                if (editing == 1) { ImGui::SetKeyboardFocusHere(); editing = 2; }
                int number = storage->GetInt(draftId, 3);
                // Coloured as the selected segment it replaces.
                const ImVec4 selectedFill(0.23f, 0.18f, 0.08f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, selectedFill);
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, selectedFill);
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, selectedFill);
                ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
                ImGui::InputInt("##customStyle", &number, 0, 0, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsDecimal);
                ImGui::PopStyleColor(4);
                storage->SetInt(draftId, number);
                DrawSettingsHelp(TR("Style number, 3 to 255. Press Enter or click away to set it."));
                // Enter or clicking elsewhere sets the number.
                if (ImGui::IsItemDeactivated()) {
                    const int value = std::clamp(number, 3, 255);
                    changed |= value != tuning.style;
                    tuning.style = value;
                    editing = 0;
                }
            }
            storage->SetInt(editId, editing);
            ImGui::PopID();
            ImGui::PopID();
            return changed;
        });
        SettingRow(TR("Intensity"), key("Intensity").c_str(), false, unavailable,
                   [&] { return ImGui::SliderFloat("##v", &tuning.intensity, 0, 2); });
        SettingRow(TR("Local tone"), key("LocalTone").c_str(), false, unavailable,
                   [&] { return ImGui::SliderFloat("##v", &tuning.localToneStrength, 0, 2); });
        SettingRow(TR("Local structure"), key("LocalStructure").c_str(), false, unavailable,
                   [&] { return ImGui::SliderFloat("##v", &tuning.localStructureStrength, 0, 2); });
        const auto skinKey = key("SkinStructure"), maskKey = key("AutoSkinMask");
        SettingRowFor(TR("Skin structure"), {skinKey.c_str(), maskKey.c_str()}, false, unavailable, [&] {
            const char* mask = TR("Auto mask");
            const float maskWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                                    ImGui::CalcTextSize(mask).x;
            ImGui::SetNextItemWidth((std::max)(Px(60.0f), ImGui::GetContentRegionAvail().x - maskWidth -
                                                                 ImGui::GetStyle().ItemSpacing.x));
            // -1 follows local structure; the slider says so.
            bool changed = ImGui::SliderFloat("##v", &tuning.skinStructureStrength, -1, 2,
                                              tuning.skinStructureStrength <= -0.999f ? TR("Follows local") : "%.3f");
            DrawSettingsHelp(TR("Structure on skin. At -1 it follows Local structure. Ctrl-click to type a value."));
            ImGui::SameLine();
            changed |= ImGui::Checkbox(mask, &tuning.useAutoSkinMask);
            DrawSettingsHelp(TR("Finds skin automatically for Skin structure."));
            return changed;
        });
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (BeginSettingRows("passInput", passLabel))
    {
        auto extent = reconstruction;
        extent.inputScale = scale;
        // The slider shows the model's input size beside the percentage.
        const auto format = width && height ? std::format("%.1f%% | {} x {}", NR::ModelExtent(width, extent),
                                                          NR::ModelExtent(height, extent))
                                            : std::string("%.1f%%");
        SettingRow(TR("Input"), key("InputScale").c_str(), true, unavailable, [&] {
            float percent = scale * 100;
            const bool changed = ImGui::SliderFloat("##v", &percent, 25, 100, format.c_str());
            if (changed) { scale = percent / 100; }
            DrawSettingsHelp(TR("NR input resolution, relative to the selected stage's scene size, and the model input it "
                                "gives. Apply to update."));
            return changed;
        });
        SettingRow(TR("Network"), key("Network").c_str(), true, unavailable, [&] {
            // 310.8 has only the old model, so it runs Old whichever is saved.
            const bool legacy = LegacyNeuralRuntime(view);
            int index = legacy || network != 0 ? 1 : 0;
            const bool changed = Segmented("network", index, {
                {TR("Small"), !legacy, legacy ? TR("310.9.16 only. 310.8 always runs the old model.")
                                          : TR("310.9.16's small FP8 model: faster and softer. NVIDIA's "
                                               "CC_SMALL_FP8; Preset B in OptiScaler forks.")},
                {TR("Old"), true, TR("The model 310.8 runs, also included in 310.9.16. NVIDIA's CC_SILVER_AARDWOLD; "
                              "Preset A in OptiScaler forks.")}});
            if (changed) { network = index; }
            DrawSettingsHelp(TR("NR network, separate from the DLSS model preset."));
            return changed;
        });
        SettingRowFor(TR("Peripheral compression"), {"PeripheralCompression", "CombinedPreparation"}, true, unavailable, [&] {
            bool changed = ImGui::Checkbox("##peripheral", &reconstruction.peripheralCompression);
            DrawSettingsHelp(TR("Peripheral compression preserves sampling density across the central 80% of each axis and "
                             "compresses the edges. Uses about 19% fewer model pixels at the same input resolution. Pass 1 "
                             "compresses the image once and the later passes work on it, so it applies to every pass. "
                             "Inspect edge quality when moving."));
            ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
            changed |= ImGui::Checkbox(TR("Combined preparation"), &reconstruction.fusedPreparation);
            DrawSettingsHelp(TR("Combines colour encoding with downsampling where needed, and depth/motion packing with "
                             "peripheral compression. Some configurations have no passes to combine. Applies to every "
                             "pass."));
            return changed;
        });
        // UI correction applies only after upscaling without Community Shaders.
        const bool noUICorrection = cs || draft.neuralBeforeUpscaling;
        SettingRow(TR("UI correction"), key("UICorrection").c_str(), false, unavailable || noUICorrection, [&] {
            const bool changed = ImGui::Checkbox("##v", &tuning.uiCorrection);
            DrawSettingsHelp(cs ? TR("NVIDIA's UI correction flag. Asks NR to treat the area around the HUD separately. Only "
                                     "used when NR runs after upscaling. Community Shaders draws the UI after NR, so it is off.")
                                : TR("NVIDIA's UI correction flag. Asks NR to treat the area around the HUD separately. Only "
                                     "used when NR runs after upscaling."));
            return changed;
        });
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::PushID(selected + 1);
    DrawPassObjects(view, draft.neuralObjectMasks, static_cast<unsigned>(selected), unavailable, decor != nullptr);
    ImGui::PopID();
    if (extraPass)
    {
        // When to run Pass 1 alone. They only ever turn the extra passes off, so
        // every extra pass's tab shows the same shared settings.
        ImGui::Spacing();
        if (BeginSettingRows("passSkip", passLabel))
        {
            SettingRowFor(TR("Skip during"), {"OnePassInCombat", "OnePassWeaponsDrawn"}, false, unavailable, [&] {
                const char* help = TR("Temporarily runs Pass 1 alone while the condition is active. Keeps your NR "
                                      "resolution, tuning and saved pass count. The image may change when switching.");
                bool changed = ImGui::Checkbox(TR("Combat"), &draft.neuralCombat.inCombat);
                DrawSettingsHelp(help);
                ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
                changed |= ImGui::Checkbox(TR("Weapons drawn"), &draft.neuralCombat.weaponsDrawn);
                DrawSettingsHelp(help);
                return changed;
            });
            SettingRow(TR("Resume after"), "ReturnDelay", false, unavailable || !draft.neuralCombat.Enabled(), [&] {
                const bool changed = ImGui::SliderFloat("##v", &draft.neuralCombat.recoverySeconds, 0.0f, 30.0f, TR("%.1f s"));
                DrawSettingsHelp(draft.neuralCombat.Enabled()
                                     ? TR("Waits this long after the conditions clear before running the other passes "
                                          "again. Pausing the game pauses the delay.")
                                     : TR("Tick Combat or Weapons drawn first."));
                return changed;
            });
            ImGui::EndTable();
        }
    }
    EndPassPanel(panel);
    if (comparePasses) { ActivePresetDecor() = nullptr; }
    if (extraPass && shown != before)
    {
        auto& own = draft.neuralExtraPasses[shownExtra];
        own = shown;
        own.linked = false;
    }
    // Reset to Pass 1 relinks only the pass whose tab offered it.
    if (action == PassAction::FollowPass1 && extraIndex >= 0) { draft.neuralExtraPasses[extraIndex].linked = true; }

    ImGui::Spacing();
    if (BeginSettingRows("sharpening", label))
    {
        SettingRowFor(TR("Sharpening"), {"SharpeningEnabled", "Sharpness"}, false, cs, [&] {
            bool changed = ImGui::Checkbox("##enabled", &sharpening);
            DrawSettingsHelp(cs ? TR("Community Shaders controls sharpening.") : TR("Applies with or without NR."));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::BeginDisabled(!sharpening);
            changed |= ImGui::SliderFloat("##strength", &sharpness, 0, 1, "%.2f");
            ImGui::EndDisabled();
            DrawSettingsHelp(cs ? TR("Community Shaders controls sharpening.") : TR("Sharpening strength."));
            return changed;
        });
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (BeginSettingRows("reconstruction", label))
    {
        SettingRow(TR("Reconstruction"), "ReconstructionMethod", true, unavailable || producerColor, [&] {
            int method = std::clamp(static_cast<int>(reconstruction.method), 0, 2);
            const bool changed = Segmented("method", method, {
                {TR("Auto"), true, TR("Reconstructs reduced input.")},
                {TR("Residual"), true, TR("Adds NR's changes; clamps colour to 0..1.")},
                {TR("Ratio"), true, TR("Transfers lighting and colour; use for linear HDR input.")}});
            if (changed) { reconstruction.method = static_cast<NR::ResolveMethod>(method); }
            DrawSettingsHelp(producerColor ? TR("CS restores scene colour automatically before DLSS.")
                                           : TR("Residual clamps colour to 0..1. Use Ratio for linear HDR input."));
            return changed;
        });
        // Ratio's settings stay visible, greyed, until Ratio is selected.
        const bool ratioOff = unavailable || (!producerColor && reconstruction.method != NR::ResolveMethod::Ratio);
        const char* ratioHelp = producerColor ? "" : TR(" Used by Ratio reconstruction.");
        SettingRowFor(TR("Effect \xC2\xB7 colour"), {"EffectStrength", "ColourStrength"}, false, ratioOff, [&] {
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::SetNextItemWidth(half);
            bool changed = ImGui::SliderFloat("##effect", &reconstruction.transferStrength, 0, 2);
            DrawSettingsHelp(std::format("{}{}", producerColor ? TR("Effect strength.") : TR("Ratio effect strength."), ratioHelp).c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::SliderFloat("##colour", &reconstruction.colourStrength, 0, 2);
            DrawSettingsHelp(std::format("{}{}", producerColor ? TR("Colour strength.") : TR("Ratio colour strength."), ratioHelp).c_str());
            return changed;
        });
        // Applied after NR, like Effect and colour; changing them keeps NR's history.
        const bool bandsOff = unavailable || producerColor || reconstruction.method != NR::ResolveMethod::Ratio;
        SettingRowFor(TR("Broad \xC2\xB7 detail"), {"BroadStrength", "DetailStrength"}, false, bandsOff, [&] {
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            const char* reason = producerColor ? TR("Not on the Community Shaders route yet.") : TR("Used by Ratio reconstruction.");
            ImGui::SetNextItemWidth(half);
            bool changed = ImGui::SliderFloat("##broad", &reconstruction.broadStrength, 0, 2);
            DrawSettingsHelp(bandsOff ? reason : TR("Broad: how much NR relights whole areas and objects. Lower it if shadows on "
                                                    "objects change as the view changes."));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::SliderFloat("##detail", &reconstruction.detailStrength, 0, 2);
            DrawSettingsHelp(bandsOff ? reason : TR("Detail: NR's local contrast and fine shading."));
            return changed;
        });
        SettingRow(producerColor ? TR("Maximum scene gain") : TR("Maximum luma ratio"), "MaximumRatio", false, ratioOff,
                   [&] { return ImGui::SliderFloat("##v", &reconstruction.maxRatio, producerColor ? 1.0f : 0.01f, 16); });
        if (!producerColor)
        {
            SettingRow(TR("HDR input"), "InputHDR", true, ratioOff, [&] {
                const bool changed = ImGui::Checkbox("##hdr", &reconstruction.colorIsHDR);
                DrawSettingsHelp(TR("Input colour is linear HDR. Used by Ratio reconstruction; it does not change "
                                 "the display HDR mode."));
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", TR("linear HDR"));
                return changed;
            });
            // How linear HDR is scaled for NR's view. Ratio's result is applied to
            // the untouched frame, so neither choice changes its brightness.
            const bool exposureOff = ratioOff || !reconstruction.colorIsHDR;
            SettingRow(TR("Exposure"), "AdaptiveExposure", false, exposureOff, [&] {
                int mode = reconstruction.adaptiveExposure ? 1 : 0;
                bool changed = Segmented("exposure", mode, {
                    {TR("Fixed"), true, TR("Divides linear HDR input by the white point before NR sees it.")},
                    {TR("Adaptive"), true, TR("TRP measures the frame's average brightness and adapts over about a second, so "
                                       "NR sees interiors and exteriors at a similar brightness. Only NR's view "
                                       "changes; its effect is carried back onto the untouched frame. NR may relight "
                                       "objects as the view's brightness changes; Fixed keeps its view steady.")}});
                if (changed) { reconstruction.adaptiveExposure = mode == 1; }
                DrawSettingsHelp(exposureOff ? TR("Used by Ratio reconstruction with linear HDR input.")
                                             : TR("How linear HDR input is scaled for NR's view."));
                return changed;
            });
            // The value row follows the choice: a fixed white point or an EV bias.
            if (reconstruction.adaptiveExposure)
            {
                SettingRow(TR("Bias"), "ExposureBias", false, exposureOff, [&] {
                    const bool changed = ImGui::SliderFloat("##bias", &reconstruction.exposureBias, -2, 2, TR("%+.1f EV"));
                    DrawSettingsHelp(exposureOff ? TR("Used by Ratio reconstruction with linear HDR input.")
                                                 : TR("Brightens NR's view when positive, darkens it when negative."));
                    return changed;
                });
            }
            else
            {
                SettingRow(TR("White point"), "WhitePoint", false, exposureOff, [&] {
                    const bool changed = ImGui::InputFloat("##white", &reconstruction.whitePoint, 0, 0, "%.4f");
                    DrawSettingsHelp(exposureOff ? TR("Used by Ratio reconstruction with linear HDR input.")
                                                 : TR("White point for encoding linear HDR input before model evaluation."));
                    return changed;
                });
            }
        }
        else SettingRowFor(TR("Scene normalization"), {"InputHDR", "WhitePoint"}, true, ratioOff, [&] {
            ImGui::BeginDisabled(producerColor);
            bool changed = ImGui::Checkbox("##hdr", &reconstruction.colorIsHDR);
            ImGui::EndDisabled();
            DrawSettingsHelp(producerColor ? TR("Community Shaders supplies scene colour; the input is not switched here.")
                                           : TR("Input colour is linear HDR. Used by Ratio reconstruction; it does not change "
                                                "the display HDR mode."));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", TR("white point"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::InputFloat("##white", &reconstruction.whitePoint, 0, 0, "%.4f");
            DrawSettingsHelp(producerColor ? TR("Scene normalization sets the brightness range NR sees.")
                                           : TR("White point for encoding linear HDR input before model evaluation."));
            return changed;
        });
        ImGui::EndTable();
    }
}

// Which nvngx_dlssnr*.dll NR loads, after the Neural Rendering checkbox. It stays
// editable while NR is unavailable, so a missing runtime can be swapped for an
// installed one; Apply switches it. While Automatic runs, the closed dropdown
// also names the runtime it chose.
bool DrawNeuralRuntimeCombo(const OverlayUI::FrameView& view, TheosRenderPipeline::SourceDLSSG::Preferences& draft,
                            float width)
{
    using TheosRenderPipeline::SourceDLSSG::NeuralRuntimeLabel;
    const auto& applied = view.neuralApplied;
    const bool automatic = TheosRenderPipeline::SourceDLSSG::IsAutomaticNeuralRuntime(draft.neuralRuntime);
    const auto chosen = automatic && applied.automaticRuntime && applied.enabled
                            ? NeuralRuntimeLabel(applied.runtimePath.string())
                            : std::string();
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##runtime", nullptr, ImGuiComboFlags_CustomPreview))
    {
        for (const auto& runtime : view.neuralRuntimes)
        {
            ImGui::PushID(runtime.c_str());
            const bool selected = runtime == draft.neuralRuntime;
            if (ImGui::Selectable(NeuralRuntimeLabel(runtime).c_str(), selected) && !selected)
            {
                draft.neuralRuntime = runtime;
                changed = true;
            }
            if (selected) { ImGui::SetItemDefaultFocus(); }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip(TR("File: %s"), runtime.c_str()); }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const auto help = (chosen.empty() ? std::string() : TRF("Automatic is using {}.", chosen) + "\n\n") +
        TR("NR runtime. Automatic chooses the runtime for your GPU. Universal unlocks NVIDIA "
           "310.9.16 on RTX 20/30/40; RTX 50 uses it natively. The kernel pack uses FP16 fallback on RTX 20/30 and FP8 on "
           "RTX 40. Legacy INT8 is an RTX 20/30 alternative. Hover an entry for its filename.");
    DrawSettingsHelp(help.c_str());
    if (ImGui::BeginComboPreview())
    {
        // One line, clipped with an ellipsis before the arrow.
        const auto label = NeuralRuntimeLabel(draft.neuralRuntime);
        const auto detail = chosen.empty() ? std::string() : "  |  " + chosen;
        const auto start = ImGui::GetCursorScreenPos();
        const float right = ImGui::GetItemRectMax().x - ImGui::GetFrameHeight() - ImGui::GetStyle().FramePadding.x;
        const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        const ImVec2 end(right, start.y + ImGui::GetTextLineHeight());
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), start, end, right, right, label.c_str(), nullptr, nullptr);
        if (!detail.empty() && start.x + labelWidth < right)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(start.x + labelWidth, start.y), end, right, right,
                                      detail.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
        }
        ImGui::EndComboPreview();
    }
    return changed;
}

void DrawSourceNeuralControls(const OverlayUI::FrameView& view, TheosRenderPipeline::SourceDLSSG::Preferences& draft,
                              bool& sharpening, float& sharpness, int upscaleType, bool nrRuntimePresent)
{
    const auto& state = view.neuralState;
    if (view.neuralPascal) { ImGui::TextWrapped("%s", TR("GTX 10 compatibility kernels (experimental)")); }
    const char* unavailableReason = NeuralUnavailableReason(view, upscaleType, nrRuntimePresent);
    const bool unavailable = unavailableReason != nullptr;
    const auto& applied = view.neuralApplied;
    if (unavailable)
    {
        ImGui::TextWrapped("%s", unavailableReason);
        ImGui::TextWrapped(TR("NR runtime path: %s"), view.neuralRuntimePath.c_str());
    }
    if (applied.enabled && !state.active && !unavailable)
    {
        ImGui::TextWrapped("%s", state.status.c_str());
    }
    if (!state.fallback.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kOchre);
        ImGui::TextWrapped("%s", state.fallback.c_str());
        ImGui::PopStyleColor();
    }
    DrawNeuralSettings(view, draft, sharpening, sharpness, unavailable);
}
} // namespace

void OverlayUI::DrawNeuralRenderingPanel(float height, const FrameView& view)
{
    if (!ImGui::BeginTabItem(TRID("Neural Rendering").c_str(), nullptr,
                             requestedPage == SettingsPage::NeuralRendering ? ImGuiTabItemFlags_SetSelected : 0))
        return;
    if (BeginSettingsColumns("neural", height, view))
    {
        const auto& applied = view.neuralApplied;
        const auto& state = view.sourceNeural;
        DrawStatusLabel(state.failed      ? TR("NR failed")
                        : state.active    ? TR("NR active")
                        : applied.enabled ? TR("NR inactive")
                                          : TR("NR off"),
                        state.failed   ? UIHealth::kError
                        : state.active ? UIHealth::kHealthy
                                       : UIHealth::kIdle);
        const auto& timing = state.telemetry;
        const auto interval = TheosRenderPipeline::NeuralRendering::ModelRateInterval(
            TheosRenderPipeline::NeuralRendering::EffectiveModelRate(applied.reconstruction.modelRate));
        const bool alternate = interval > 1;
		DrawSettingsValue(TR("NR per frame"), state.active && timing.frameSamples
			? TRF("{:.2f} ms", timing.AverageFrameGPUMicroseconds() / 1000.0).c_str() : TR("unavailable"));
		DrawSettingsHelp(TR("Measured NR GPU work per rendered frame, averaged over the latest completed samples, including "
			"model runs, skipped-frame reprojection, change storage, preparation, reconstruction, previews and NR UI composition. "
			"Excludes object classification capture, Freeze frame capture, the D3D11/D3D12 handoff and frame generation."));
        DrawSettingsValue(TR("Inference"), state.active && timing.gpuSamples
                                           ? (alternate ? TRF("{:.2f} ms per run", timing.AverageGPUMicroseconds() / 1000.0)
                                                        : TRF("{:.2f} ms", timing.AverageGPUMicroseconds() / 1000.0)).c_str()
                                           : TR("unavailable"));
        DrawSettingsHelp(alternate ? TR("Combined model inference and inter-pass preparation, per frame the model runs on. "
                                        "Frames between skip inference but still do reprojection and storage. See NR per frame for measured work. Excludes input "
                                        "preparation, the last pass's restoration, reconstruction, UI composition and the "
                                        "D3D11/D3D12 handoff.")
                                   : TR("Combined model inference and inter-pass preparation. Excludes input preparation, the "
                                        "last pass's restoration, reconstruction, UI composition and the D3D11/D3D12 handoff."));
        if (alternate)
        {
            const char* rateName = interval == 3 ? TR("Every third frame") : TR("Every other frame");
            DrawSettingsValue(TR("Model rate"), state.active ? TRF("{} | {:.0f}% of frames", rateName,
                                                                   timing.modelRunShare * 100.0).c_str()
                                                             : rateName);
            DrawSettingsHelp(TR("The share of recent frames the model ran on. Resets, setting changes and Freeze frame "
                                "run it on every frame."));
        }
        if (state.active)
            DrawStageMeasurements(view, SettingsPage::NeuralRendering);
        DrawNRAppliedPasses(view, applied);
        if (state.active) {
            const auto count = state.effectivePasses == 1 ? std::string(TR("1 pass")) : TRF("{} passes", state.effectivePasses);
            const auto execution = state.passOverride == TheosRenderPipeline::NeuralRendering::PassOverride::None ? count :
                std::format("{} - {}", count, TR(TheosRenderPipeline::NeuralRendering::PassOverrideName(state.passOverride)));
            DrawSettingsValue(TR("Running"), execution.c_str());
            // A later pass NGX would not start; the passes before it keep running.
            if (!state.passNote.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kOchre);
                ImGui::TextWrapped("%s", state.passNote.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (state.failed || (applied.enabled && !state.active))
            ImGui::TextWrapped("%s", state.status.c_str());
        if (showDeveloperControls)
        {
            ImGui::Separator();
            if (!state.failed && (!applied.enabled || state.active))
                ImGui::TextWrapped("%s", state.status.c_str());
            ImGui::TextWrapped(TR("Submitted frames %llu | history resets %llu"),
                               static_cast<unsigned long long>(state.evaluations),
                               static_cast<unsigned long long>(state.resets));
            if (timing.gpuSamples)
                ImGui::TextWrapped(TR("Inference max %.3f ms | %llu samples | %llu query failures"),
                                   timing.MaximumGPUMicroseconds() / 1000.0,
                                   static_cast<unsigned long long>(timing.gpuSamples),
                                   static_cast<unsigned long long>(timing.gpuQueryFailures));
        }
        DrawPresetList(view);
        NextSettingsColumn(height);
        if (PresetEditorSelected()) {
            const bool unavailable = NeuralUnavailableReason(view, settingsDraft.upscaleType, nrRuntimePresent) != nullptr;
            DrawPresetEditor(view, [&](TheosRenderPipeline::SourceDLSSG::Preferences& draft, bool& sharpening, float& sharpness) {
                DrawNeuralSettings(view, draft, sharpening, sharpness, unavailable);
            });
        }
        else {
            DrawBaseSettings(view, [&] {
                g_setCompare = [this](auto compare) { SetNeuralCompare(compare); };
                g_labModelRate = showDeveloperControls;
                DrawSourceNeuralControls(view, settingsDraft.sourceDLSSG, settingsDraft.sharpening, settingsDraft.sharpness,
                                         settingsDraft.upscaleType, nrRuntimePresent);
                g_setCompare = nullptr;
                g_labModelRate = false;
            });
        }
        EndSettingsColumns();
    }
    ImGui::EndTabItem();
}
