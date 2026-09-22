#include "OverlayLayout.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace TheosRenderPipeline::Overlay
{
ColumnSizes DrawColumnSplitter(float width, float height, float& leftFraction)
{
    auto columns = FitColumns(width, leftFraction);
    const float available = columns.left + columns.right;
    const auto origin = ImGui::GetCursorScreenPos();
    const ImRect bounds(ImVec2(origin.x + columns.left, origin.y),
                        ImVec2(origin.x + columns.left + ColumnGap, origin.y + height));
    if (ImGui::SplitterBehavior(bounds, ImGui::GetID("##columnDivider"), ImGuiAxis_X, &columns.left, &columns.right,
                                (std::min)(260.0f, available * 0.4f), (std::min)(350.0f, available * 0.5f)))
    {
        leftFraction = std::clamp(columns.left / available, 0.2f, 0.8f);
        columns = FitColumns(width, leftFraction);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Drag to resize columns. Save as default remembers the layout.");
    }
    const float x = origin.x + columns.left + ColumnGap * 0.5f;
    const auto color = ImGui::GetColorU32(ImGui::IsItemActive()    ? ImGuiCol_SeparatorActive
                                          : ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered
                                                                   : ImGuiCol_Separator);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), color);
    return columns;
}
} // namespace TheosRenderPipeline::Overlay
