#pragma once

#include <array>
#include <imgui.h>

namespace TheosRenderPipeline::Overlay
{
	// Polled numeric/editing input only. No window subclass or input-thread
	// ImGui calls. The owner supplies Win32 virtual-key state on the render thread.
	class NumericInput
	{
	public:
		using Keys = std::array<bool, 256>;
		void Update(ImGuiIO& io, const Keys& keys, bool focused, bool capturingHotkey = false)
		{
            if (focused != applicationFocused_) { io.AddFocusEvent(focused); }
            applicationFocused_ = focused;
            if (focused && capturingHotkey) {
                // Suspend numeric keys without declaring application focus lost.
                // Reset our edge state so held capture keys are blocked when
                // ordinary editing resumes.
                Release(io);
                return;
            }
			if (!focused) {
				Release(io);
				return;
			}
			if (!focused_) {
				// Do not type a key held while opening the overlay or alt-tabbing in.
				blocked_ = keys;
				focused_ = true;
			}
			Keys current{};
			for (unsigned vk = 0; vk < current.size(); ++vk) {
				if (!keys[vk]) { blocked_[vk] = false; }
				current[vk] = keys[vk] && !blocked_[vk];
			}
			io.AddKeyEvent(ImGuiMod_Ctrl, current[0x11]);
			io.AddKeyEvent(ImGuiMod_Shift, current[0x10]);
			io.AddKeyEvent(ImGuiMod_Alt, current[0x12]);
			io.AddKeyEvent(ImGuiMod_Super, current[0x5B] || current[0x5C]);
			for (const auto& key : editingKeys_) {
				io.AddKeyEvent(key.imgui, current[key.vk]);
			}
			for (unsigned digit = 0; digit <= 9; ++digit) {
				for (const auto base : { 0x30u, 0x60u }) {
					const auto vk = base + digit;
					const auto key = static_cast<ImGuiKey>((base == 0x30 ? ImGuiKey_0 : ImGuiKey_Keypad0) + digit);
					io.AddKeyEvent(key, current[vk]);
					if (current[vk] && !previous_[vk] && io.WantTextInput &&
						!current[0x11] && !current[0x12] && !current[0x5B] && !current[0x5C] &&
						(base == 0x60 || !current[0x10])) {
						io.AddInputCharacter('0' + digit);
					}
				}
			}
			previous_ = current;
		}

		void Release(ImGuiIO& io)
		{
			for (const auto& key : editingKeys_) { io.AddKeyEvent(key.imgui, false); }
			for (int digit = 0; digit <= 9; ++digit) {
				io.AddKeyEvent(static_cast<ImGuiKey>(ImGuiKey_0 + digit), false);
				io.AddKeyEvent(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + digit), false);
			}
			for (const auto modifier : { ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super }) {
				io.AddKeyEvent(modifier, false);
			}
			previous_ = {};
			blocked_ = {};
			focused_ = false;
		}

	private:
		struct Key { unsigned vk; ImGuiKey imgui; };
		static constexpr Key editingKeys_[]{
			{ 0x08, ImGuiKey_Backspace }, { 0x09, ImGuiKey_Tab }, { 0x0D, ImGuiKey_Enter },
			{ 0x1B, ImGuiKey_Escape }, { 0x24, ImGuiKey_Home },
			{ 0x25, ImGuiKey_LeftArrow }, { 0x27, ImGuiKey_RightArrow }, { 0x2E, ImGuiKey_Delete },
			{ 'A', ImGuiKey_A }, { 'C', ImGuiKey_C }, { 'V', ImGuiKey_V },
			{ 'X', ImGuiKey_X }, { 'Y', ImGuiKey_Y }, { 'Z', ImGuiKey_Z }
		};
		Keys previous_{}, blocked_{};
		bool focused_{};
        bool applicationFocused_{};
	};

	inline bool FPSInput(const char* id, int& value)
	{
		ImGui::SetNextItemWidth(-1.0f);
		// Step zero removes the small +/- buttons and leaves a full-width textbox.
		return ImGui::InputInt(id, &value, 0, 0, ImGuiInputTextFlags_AutoSelectAll);
	}
}
