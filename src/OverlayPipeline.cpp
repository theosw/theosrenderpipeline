#include "OverlayPipeline.h"
#include "OverlayUIStyle.h"

#include <algorithm>
#include <string>

namespace TheosRenderPipeline::Overlay
{
SettingsPage DrawPipelineDiagram(const PipelineDiagram& diagram)
{
    const auto origin = ImGui::GetCursorScreenPos();
    const auto& style = ImGui::GetStyle();
    const float width = ImGui::GetContentRegionAvail().x;
    const float lineHeight = ImGui::GetTextLineHeight();
    const float padding = style.FramePadding.x;
    const float gap = lineHeight * 2.0f;
    const float stageCount = static_cast<float>(diagram.stages.size());
    const float nodeWidth = (width - gap * (stageCount - 1.0f)) / stageCount;
    const float textWidth = (std::max)(1.0f, nodeWidth - padding * 2.0f);
    std::string uiLabel = std::string("Native UI | ") + (diagram.nativeUI ? diagram.nativeDetail : "off");
    if (ImGui::CalcTextSize(uiLabel.c_str()).x > textWidth)
    {
        uiLabel.replace(uiLabel.find(" | "), 3, "\n");
    }
    const auto uiTextSize = ImGui::CalcTextSize(uiLabel.c_str(), nullptr, false, textWidth);
    const float headerHeight = uiTextSize.y + style.FramePadding.y * 2.0f;
    std::array<std::string, PipelineDiagram::StageCount> details;
    float detailHeight = lineHeight;
    for (std::size_t i = 0; i < diagram.stages.size(); ++i)
    {
        details[i] = diagram.stages[i].detail;
        const auto separator = details[i].find(" | ");
        if (separator != std::string::npos && ImGui::CalcTextSize(details[i].c_str()).x > textWidth)
        {
            details[i].replace(separator, 3, "\n");
        }
        detailHeight = (std::max)(detailHeight, ImGui::CalcTextSize(details[i].c_str(), nullptr, false, textWidth).y);
    }
    const float nodeHeight = style.FramePadding.y * 2.0f + lineHeight + style.ItemInnerSpacing.y + detailHeight;
    const float nodeTop = origin.y + headerHeight + style.ItemSpacing.y;
    const float middle = nodeTop + nodeHeight * 0.5f;
    const float height = headerHeight + style.ItemSpacing.y + nodeHeight;
    auto* draw = ImGui::GetWindowDrawList();
    const auto amber = ImGui::GetColorU32(kAmber);
    const auto muted = ImGui::GetColorU32(kMuted);
    const auto ivory = ImGui::GetColorU32(kIvory);
    SettingsPage requested = SettingsPage::None;
    ImGui::PushID("pipelineDiagram");
    ImGui::BeginGroup();

    // UI rejoins the scene after either NR placement and before generation.
    const float uiWidth = nodeWidth;
    const auto uiEnd = ImVec2(origin.x + uiWidth, origin.y + headerHeight);
    draw->AddRectFilled(origin, uiEnd, ImGui::GetColorU32(kPanel), style.FrameRounding);
    draw->AddRect(origin, uiEnd, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);
    draw->PushClipRect(origin, uiEnd, true);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                  ImVec2(origin.x + (uiWidth - uiTextSize.x) * 0.5f, origin.y + style.FramePadding.y),
                  diagram.nativeUI ? ivory : muted, uiLabel.c_str(), nullptr, textWidth);
    draw->PopClipRect();
    ImGui::Dummy(ImVec2(uiWidth, headerHeight));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Native-resolution menus and HUD join the processed scene before frame generation.\nThe "
                          "diagram shows applied settings; controls below may contain unapplied edits.");
    }

    const float branchX = origin.x + static_cast<float>(PipelineDiagram::GenerationStage) * (nodeWidth + gap) - gap * 0.5f;
    if (diagram.nativeUI)
    {
        const float branchY = origin.y + headerHeight * 0.5f;
        const float arrow = lineHeight * 0.3f;
        draw->AddLine(ImVec2(uiEnd.x, branchY), ImVec2(branchX, branchY), amber, 1.5f);
        draw->AddLine(ImVec2(branchX, branchY), ImVec2(branchX, middle - arrow), amber, 1.5f);
        draw->AddTriangleFilled(ImVec2(branchX - arrow, middle - arrow * 2.0f),
                                ImVec2(branchX + arrow, middle - arrow * 2.0f), ImVec2(branchX, middle - arrow * 0.5f),
                                amber);
    }

    const auto statusColor = ImGui::GetColorU32(diagram.statusColor);
    const float statusX = origin.x + width - ImGui::CalcTextSize(diagram.status).x;
    draw->AddCircleFilled(ImVec2(statusX - lineHeight, origin.y + headerHeight * 0.5f), 3.0f, statusColor);
    draw->AddText(ImVec2(statusX, origin.y + style.FramePadding.y), statusColor, diagram.status);

    for (std::size_t i = 0; i < diagram.stages.size(); ++i)
    {
        const auto& stage = diagram.stages[i];
        const ImVec2 start(origin.x + static_cast<float>(i) * (nodeWidth + gap), nodeTop);
        const ImVec2 end(start.x + nodeWidth, start.y + nodeHeight);
        ImGui::SetCursorScreenPos(start);
        ImGui::PushID(static_cast<int>(i));
        if (stage.page != SettingsPage::None)
        {
            if (ImGui::InvisibleButton("stage", ImVec2(nodeWidth, nodeHeight)))
            {
                requested = stage.page;
            }
        }
        else
        {
            ImGui::Dummy(ImVec2(nodeWidth, nodeHeight));
        }
        const bool hovered = ImGui::IsItemHovered();
        const bool interactiveHover = hovered && stage.page != SettingsPage::None;
        draw->AddRectFilled(start, end,
                            ImGui::GetColorU32(interactiveHover ? ImGuiCol_FrameBgHovered : ImGuiCol_ChildBg),
                            style.FrameRounding);
        draw->AddRect(start, end, interactiveHover ? amber : ImGui::GetColorU32(ImGuiCol_Border),
                      style.FrameRounding);
        draw->PushClipRect(start, end, true);
        const auto titleSize = ImGui::CalcTextSize(stage.title);
        draw->AddText(ImVec2(start.x + (nodeWidth - titleSize.x) * 0.5f, start.y + style.FramePadding.y),
                      stage.muted ? muted : ivory,
                      stage.title);
        const auto detailSize = ImGui::CalcTextSize(details[i].c_str(), nullptr, false, textWidth);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                      ImVec2(start.x + (nodeWidth - detailSize.x) * 0.5f,
                             start.y + style.FramePadding.y + lineHeight + style.ItemInnerSpacing.y),
                      muted, details[i].c_str(), nullptr, textWidth);
        draw->PopClipRect();
        if (hovered)
        {
            ImGui::SetTooltip("%s", stage.tooltip);
            if (stage.page != SettingsPage::None)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }
        ImGui::PopID();
        if (i + 1 < diagram.stages.size())
        {
            const float arrow = lineHeight * 0.3f;
            const float tip = end.x + gap - 3.0f;
            draw->AddLine(ImVec2(end.x, middle), ImVec2(tip - arrow, middle), amber, 1.5f);
            draw->AddTriangleFilled(ImVec2(tip - arrow, middle - arrow), ImVec2(tip, middle),
                                    ImVec2(tip - arrow, middle + arrow), amber);
        }
    }
    if (diagram.nativeUI)
    {
        draw->AddCircleFilled(ImVec2(branchX, middle), 3.0f, ImGui::GetColorU32(kPanel));
        draw->AddCircle(ImVec2(branchX, middle), 3.0f, amber, 12, 1.5f);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::EndGroup();
    ImGui::PopID();
    ImGui::PushStyleColor(ImGuiCol_Separator, kAmberDim);
    ImGui::Separator();
    ImGui::PopStyleColor();
    return requested;
}
} // namespace TheosRenderPipeline::Overlay
