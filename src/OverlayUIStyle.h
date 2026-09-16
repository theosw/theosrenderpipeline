#pragma once

#include <imgui.h>

namespace TheosRenderPipeline::Overlay
{
	inline const ImVec4 kIvory{ 0.91f, 0.89f, 0.84f, 1.0f };
	inline const ImVec4 kMuted{ 0.56f, 0.57f, 0.55f, 1.0f };
	inline const ImVec4 kAmber{ 0.84f, 0.60f, 0.17f, 1.0f };
	inline const ImVec4 kAmberDim{ 0.45f, 0.31f, 0.10f, 1.0f };
	inline const ImVec4 kSage{ 0.50f, 0.65f, 0.42f, 1.0f };
	inline const ImVec4 kOchre{ 0.85f, 0.64f, 0.23f, 1.0f };
	inline const ImVec4 kRust{ 0.77f, 0.37f, 0.30f, 1.0f };
	inline const ImVec4 kPanel{ 0.075f, 0.082f, 0.086f, 0.98f };

	enum class UIHealth
	{
		kIdle,
		kHealthy,
		kWarning,
		kError
	};

    const char* ModeName(int mode);

	ImVec4 HealthColor(UIHealth a_health);
	void ApplyRendererStyle();
	void DrawHealthDot(UIHealth a_health);
	void DrawStatusLabel(const char* a_label, UIHealth a_health);
	void DrawBadge(const char* a_label, const ImVec4& a_color);
}
