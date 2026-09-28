#pragma once

#include <array>
#include <imgui.h>

namespace TheosRenderPipeline::Overlay
{
	// Polled editing input. No window subclass or input-thread ImGui calls. The
	// owner supplies Win32 virtual-key state on the render thread, and optionally
	// a layout translator for text characters beyond the digits typed here.
	class NumericInput
	{
	public:
		using Keys = std::array<bool, 256>;
		void Update(ImGuiIO& io, const Keys& keys, bool focused)
		{
			Update(io, keys, focused, [](unsigned, const Keys&, auto&&) {});
		}
		// translate(vk, keys, emit) calls emit(wchar_t) for each character the key types.
		template <class Translate>
		void Update(ImGuiIO& io, const Keys& keys, bool focused, Translate&& translate)
		{
			if (focused != focused_) { io.AddFocusEvent(focused); }
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
						!current[0x11] && !current[0x12] && !current[0x5B] && !current[0x5C]) {
						// The number row types what the layout gives, such as e-acute on French AZERTY;
						// without a layout translator it types the digit, as numeric fields expect.
						int typed = 0;
						if (base == 0x30) {
							translate(vk, current, [&](wchar_t c) {
								if (c >= 0x20 && c != 0x7F) { io.AddInputCharacterUTF16(c); ++typed; }
							});
						}
						if (!typed && (base == 0x60 || !current[0x10])) { io.AddInputCharacter('0' + digit); }
					}
				}
			}
			if (io.WantTextInput) {
				const bool ctrl = current[0x11], alt = current[0x12], win = current[0x5B] || current[0x5C];
				// Ctrl/Alt/Win shortcuts do not type; Ctrl+Alt is AltGr on many layouts.
				if (!win && ctrl == alt) {
					for (unsigned vk = 0x20; vk < current.size(); ++vk) {
						if (!current[vk] || previous_[vk] || !Printable(vk) || TypedAsDigit(vk, current)) { continue; }
						translate(vk, current, [&](wchar_t c) {
							if (c >= 0x20 && c != 0x7F) { io.AddInputCharacterUTF16(c); }
						});
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
		static bool Printable(unsigned vk)
		{
			return vk == 0x20 || (vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A) ||
				(vk >= 0x60 && vk <= 0x6F && vk != 0x6C) || (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF) || vk == 0xE2;
		}
		// Keys the number loop above already typed: the keypad digits, and the number
		// row except with AltGr (Ctrl+Alt), which that loop leaves to the layout here.
		static bool TypedAsDigit(unsigned vk, const Keys& keys)
		{
			return (vk >= 0x60 && vk <= 0x69) || (vk >= 0x30 && vk <= 0x39 && !(keys[0x11] && keys[0x12]));
		}
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
	};

	inline bool FPSInput(const char* id, int& value)
	{
		ImGui::SetNextItemWidth(-1.0f);
		// Step zero removes the small +/- buttons and leaves a full-width textbox.
		return ImGui::InputInt(id, &value, 0, 0, ImGuiInputTextFlags_AutoSelectAll);
	}
}
