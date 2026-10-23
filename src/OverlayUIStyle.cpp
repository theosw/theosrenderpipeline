#include "OverlayUIStyle.h"
#include "Localization.h"
#include "UpscaleType.h"

namespace TheosRenderPipeline::Overlay
{
	namespace
	{
		constexpr float kBaseFontPixels = 14.0f;
		float g_uiScale = 1.0f;

		std::span<const std::byte> EmbeddedFont()
		{
			HMODULE module{};
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&EmbeddedFont), &module)) {
				return {};
			}
			const auto resource = ::FindResourceW(module, L"TRP_OVERLAY_FONT", MAKEINTRESOURCEW(10));  // RT_RCDATA
			const auto loaded = resource ? ::LoadResource(module, resource) : nullptr;
			const auto data = loaded ? ::LockResource(loaded) : nullptr;
			if (!data) {
				return {};
			}
			return { static_cast<const std::byte*>(data), ::SizeofResource(module, resource) };
		}
	}

	float UIScale()
	{
		return g_uiScale;
	}

	namespace
	{
		// %WINDIR%\Fonts as UTF-8: Dear ImGui opens font files by UTF-8 path.
		std::string WindowsFontDirectory()
		{
			std::wstring buffer(MAX_PATH + 1, L'\0');
			const UINT length = ::GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
			if (length == 0 || length >= buffer.size()) {
				return {};
			}
			buffer.resize(length);
			const auto length32 = static_cast<int>(buffer.size());
			const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), length32, nullptr, 0, nullptr, nullptr);
			if (bytes <= 0) {
				return {};
			}
			std::string text(static_cast<std::size_t>(bytes), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), length32, text.data(), bytes, nullptr, nullptr);
			return text + "\\Fonts";
		}

		// The atlas copies only the pointer, and it reads the list when the font
		// texture is created, long after the merge call. A function-local list
		// would dangle there, so this one outlives every merge and is rebuilt on
		// each atlas rebuild (Clear() has already dropped the fonts that used it).
		ImVector<ImWchar> g_glyphRanges;

		// The embedded font covers Latin only, so the dictionary's extra glyphs come
		// from a system font. Merge that font instead of replacing the primary so
		// Latin text keeps the embedded metrics and layout. The language button
		// lists every installed language in its own name, so their names' glyphs
		// are merged whichever language is active.
		void AddLocalizedGlyphs(ImFontAtlas& a_fonts, float a_sizePixels)
		{
			namespace Localization = TheosRenderPipeline::Localization;
			const auto table = Localization::Active();
			std::vector<char32_t> codepoints;
			if (table) {
				codepoints = table->Codepoints();
			}
			for (const auto& language : Localization::Catalog()) {
				const auto name = Localization::Codepoints(language.name);
				codepoints.insert(codepoints.end(), name.begin(), name.end());
			}
			if (codepoints.empty()) {
				// English labels are already covered by the embedded font.
				return;
			}

			ImFontGlyphRangesBuilder builder;
			builder.AddRanges(a_fonts.GetGlyphRangesDefault());
			for (const char32_t codepoint : codepoints) {
				if (codepoint > 0x20 && codepoint <= 0xFFFF) {
					// This build's ImWchar is 16 bit, so the atlas cannot address
					// code points above the basic multilingual plane.
					builder.AddChar(static_cast<ImWchar>(codepoint));
				} else if (codepoint > 0xFFFF) {
					logger::warn("[Overlay] dictionary glyph U+{:05X} is outside the 16-bit range; skipped",
						static_cast<unsigned>(codepoint));
				}
			}
			g_glyphRanges.clear();
			builder.BuildRanges(&g_glyphRanges);

			ImFontConfig config{};
			config.MergeMode = true;
			config.GlyphRanges = g_glyphRanges.Data;

			// The active dictionary names the fonts it wants; a bare file name is
			// resolved inside the Windows font directory. Every candidate is tried in
			// order, so a font that Windows does not ship only costs one failed open.
			const auto fontDirectory = WindowsFontDirectory();
			std::vector<std::string> candidates;
			const auto add = [&](std::string_view a_name) {
				if (a_name.empty()) {
					return;
				}
				const bool absolute = a_name.find_first_of("\\/:") != std::string_view::npos;
				if (absolute || fontDirectory.empty()) {
					candidates.emplace_back(a_name);
				} else {
					candidates.push_back(fontDirectory + "\\" + std::string(a_name));
				}
			};
			if (table) {
				for (const auto& font : table->Description().fonts) {
					add(font);
				}
			}
			// Fonts Windows ships for the UI languages this plugin runs with:
			// Simplified Chinese, Japanese, Korean, then a pan-CJK fallback.
			for (const auto* font : { "msyh.ttc", "msyh.ttf", "simhei.ttf", "YuGothM.ttc", "meiryo.ttc", "malgun.ttf",
				     "arialuni.ttf" }) {
				add(font);
			}
			for (const auto& candidate : candidates) {
				if (a_fonts.AddFontFromFileTTF(candidate.c_str(), a_sizePixels, &config)) {
					return;
				}
			}
			logger::warn("[Overlay] no system font covers the dictionary's glyphs; localized labels will render as '?'");
		}
	} // namespace

	bool BuildRendererFont(float a_scale)
	{
		auto& fonts = *ImGui::GetIO().Fonts;
		fonts.Clear();
		ImFontConfig config;
		// The atlas copies the resource; Skyrim's module image stays read-only.
		config.FontDataOwnedByAtlas = false;
		const auto font = EmbeddedFont();
		if (!font.empty() && fonts.AddFontFromMemoryTTF(const_cast<std::byte*>(font.data()),
				static_cast<int>(font.size()), std::round(kBaseFontPixels * a_scale), &config)) {
			AddLocalizedGlyphs(fonts, std::round(kBaseFontPixels * a_scale));
			return true;
		}
		config = {};
		config.SizePixels = std::round(13.0f * a_scale);
		fonts.AddFontDefault(&config);
		AddLocalizedGlyphs(fonts, config.SizePixels);
		return false;
	}

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

	void ApplyRendererStyle(float a_scale)
	{
		g_uiScale = a_scale;
		auto& style = ImGui::GetStyle();
		// ScaleAllSizes compounds, so start from ImGui's unscaled defaults.
		style = ImGuiStyle();
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
		style.ScaleAllSizes(a_scale);
		// ScaleAllSizes truncates the software cursor's scale to a whole number, so
		// zoom below 100% drew it at size 0 (an invisible cursor at 75%) and zoom
		// up to 199% left it unscaled. Follow the zoom, never below its normal size.
		style.MouseCursorScale = (std::max)(1.0f, a_scale);
	}

	void DrawHealthDot(UIHealth a_health)
	{
		const auto cursor = ImGui::GetCursorScreenPos();
		const float size = ImGui::GetTextLineHeight();
		ImGui::GetWindowDrawList()->AddCircleFilled(
			ImVec2(cursor.x + size * 0.5f, cursor.y + size * 0.5f),
			Px(4.0f),
			ImGui::ColorConvertFloat4ToU32(HealthColor(a_health)));
		ImGui::Dummy(ImVec2(size, size));
	}

	void DrawStatusLabel(const char* a_label, UIHealth a_health)
	{
		DrawHealthDot(a_health);
		ImGui::SameLine(0.0f, Px(5.0f));
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

    void DrawSettingsHeading(const char* title, const char* behavior)
    {
        ImGui::Spacing();
        ImGui::SeparatorText(title);
        if (behavior && *behavior)
        {
            ImGui::TextDisabled("%s", behavior);
        }
    }

    void DrawSettingsValue(const char* label, const char* value)
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const float start = ImGui::GetCursorPosX();
        const float labelWidth = width * 0.46f;
        ImGui::PushTextWrapPos(start + labelWidth - ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextDisabled("%s", label);
        ImGui::PopTextWrapPos();
        ImGui::SameLine(start + labelWidth);
        ImGui::TextWrapped("%s", value);
    }

    void DrawSettingsHelp(const char* text)
    {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
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
    case XeSS:
        return "XeSS";
    default:
        return "DLSS";
    }
}

}
