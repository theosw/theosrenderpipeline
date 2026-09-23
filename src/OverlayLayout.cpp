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
    constexpr float lineWidth = 2.0f;
    constexpr float grabWidth = 12.0f;
    const float inset = (std::min)(8.0f, height * 0.25f);
    const float x = std::floor(origin.x + columns.left + (ColumnGap - lineWidth) * 0.5f);
    const ImRect bounds(ImVec2(x, origin.y + inset), ImVec2(x + lineWidth, origin.y + height - inset));
    // SplitterBehavior paints its bounds; extend only the hit area to keep the line easy to grab.
    if (ImGui::SplitterBehavior(bounds, ImGui::GetID("##columnDivider"), ImGuiAxis_X, &columns.left, &columns.right,
                                (std::min)(260.0f, available * 0.4f), (std::min)(350.0f, available * 0.5f),
                                (grabWidth - lineWidth) * 0.5f))
    {
        leftFraction = std::clamp(columns.left / available, 0.2f, 0.8f);
        columns = FitColumns(width, leftFraction);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Drag to resize columns. Save as default remembers the layout.");
    }
    return columns;
}
} // namespace TheosRenderPipeline::Overlay
