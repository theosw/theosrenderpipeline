#include "OverlayUIStyle.h"
#include "RenderPipeline.h"

namespace TheosRenderPipeline::Overlay
{
	ImVec4 HealthColor(UIHealth a_health)
	{
		switch (a_health) {
		case UIHealth::kHealthy:
			return kSage;
		case UIHealth::kWarning:
			return kOchre;
		case UIHealth::kError:
			return kRust;
		default:
			return kMuted;
		}
	}

	void ApplyRendererStyle()
	{
		auto& style = ImGui::GetStyle();
		style.WindowPadding = ImVec2(14.0f, 12.0f);
		style.FramePadding = ImVec2(10.0f, 6.0f);
		style.CellPadding = ImVec2(9.0f, 7.0f);
		style.ItemSpacing = ImVec2(9.0f, 7.0f);
		style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
		style.ScrollbarSize = 13.0f;
		style.WindowRounding = 4.0f;
		style.ChildRounding = 3.0f;
		style.FrameRounding = 3.0f;
		style.PopupRounding = 3.0f;
		style.ScrollbarRounding = 3.0f;
		style.GrabRounding = 3.0f;
		style.TabRounding = 3.0f;
		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.FrameBorderSize = 1.0f;

		auto* colors = style.Colors;
		colors[ImGuiCol_Text] = kIvory;
		colors[ImGuiCol_TextDisabled] = kMuted;
		colors[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.050f, 0.052f, 0.98f);
		colors[ImGuiCol_ChildBg] = kPanel;
		colors[ImGuiCol_PopupBg] = ImVec4(0.055f, 0.060f, 0.063f, 0.99f);
		colors[ImGuiCol_Border] = ImVec4(0.22f, 0.23f, 0.22f, 1.0f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
		colors[ImGuiCol_FrameBg] = ImVec4(0.105f, 0.112f, 0.115f, 1.0f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.145f, 0.105f, 1.0f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.21f, 0.17f, 0.09f, 1.0f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.045f, 0.050f, 0.052f, 1.0f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.055f, 0.060f, 0.063f, 1.0f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.045f, 0.050f, 0.052f, 1.0f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.055f, 0.060f, 0.063f, 1.0f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.045f, 0.050f, 0.052f, 1.0f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.24f, 0.22f, 1.0f);
		colors[ImGuiCol_ScrollbarGrabHovered] = kAmberDim;
		colors[ImGuiCol_ScrollbarGrabActive] = kAmber;
		colors[ImGuiCol_CheckMark] = kAmber;
		colors[ImGuiCol_SliderGrab] = kAmberDim;
		colors[ImGuiCol_SliderGrabActive] = kAmber;
		colors[ImGuiCol_Button] = ImVec4(0.12f, 0.125f, 0.125f, 1.0f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.31f, 0.23f, 0.10f, 1.0f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.46f, 0.31f, 0.08f, 1.0f);
		colors[ImGuiCol_Header] = ImVec4(0.25f, 0.19f, 0.09f, 0.75f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.34f, 0.24f, 0.09f, 0.90f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.43f, 0.29f, 0.08f, 1.0f);
		colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.23f, 0.22f, 1.0f);
		colors[ImGuiCol_SeparatorHovered] = kAmberDim;
		colors[ImGuiCol_SeparatorActive] = kAmber;
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.22f, 0.09f, 0.35f);
		colors[ImGuiCol_ResizeGripHovered] = kAmberDim;
		colors[ImGuiCol_ResizeGripActive] = kAmber;
		colors[ImGuiCol_Tab] = ImVec4(0.075f, 0.082f, 0.086f, 1.0f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.31f, 0.23f, 0.10f, 1.0f);
		colors[ImGuiCol_TabActive] = ImVec4(0.40f, 0.27f, 0.08f, 1.0f);
		colors[ImGuiCol_TabUnfocused] = ImVec4(0.065f, 0.070f, 0.073f, 1.0f);
		colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.25f, 0.19f, 0.09f, 1.0f);
		colors[ImGuiCol_PlotLines] = kAmber;
		colors[ImGuiCol_PlotLinesHovered] = kOchre;
		colors[ImGuiCol_PlotHistogram] = kAmber;
		colors[ImGuiCol_PlotHistogramHovered] = kOchre;
		colors[ImGuiCol_TableHeaderBg] = ImVec4(0.095f, 0.100f, 0.102f, 1.0f);
		colors[ImGuiCol_TableBorderStrong] = ImVec4(0.23f, 0.24f, 0.23f, 1.0f);
		colors[ImGuiCol_TableBorderLight] = ImVec4(0.16f, 0.17f, 0.17f, 1.0f);
		colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
		colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.10f, 0.105f, 0.105f, 0.40f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.46f, 0.31f, 0.08f, 0.60f);
		colors[ImGuiCol_NavHighlight] = kAmber;
	}

	void DrawHealthDot(UIHealth a_health)
	{
		const auto cursor = ImGui::GetCursorScreenPos();
		const float size = ImGui::GetTextLineHeight();
		ImGui::GetWindowDrawList()->AddCircleFilled(
			ImVec2(cursor.x + size * 0.5f, cursor.y + size * 0.5f),
			4.0f,
			ImGui::ColorConvertFloat4ToU32(HealthColor(a_health)));
		ImGui::Dummy(ImVec2(size, size));
	}

	void DrawStatusLabel(const char* a_label, UIHealth a_health)
	{
		DrawHealthDot(a_health);
		ImGui::SameLine(0.0f, 5.0f);
		ImGui::TextColored(HealthColor(a_health), "%s", a_label);
	}

	void DrawBadge(const char* a_label, const ImVec4& a_color)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, a_color);
		ImGui::PushStyleColor(ImGuiCol_Border, a_color);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(a_color.x * 0.18f, a_color.y * 0.18f, a_color.z * 0.18f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(a_color.x * 0.18f, a_color.y * 0.18f, a_color.z * 0.18f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(a_color.x * 0.18f, a_color.y * 0.18f, a_color.z * 0.18f, 1.0f));
		ImGui::SmallButton(a_label);
		ImGui::PopStyleColor(5);
	}

}

namespace TheosRenderPipeline::Overlay
{
const char* ModeName(int a_mode)
{
    switch (a_mode)
    {
    case DLAA:
        return "DLAA";
    default:
        return "DLSS";
    }
}

}
