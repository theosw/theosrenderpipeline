#include "OverlayNumericInput.h"
#include <imgui_internal.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace TheosRenderPipeline::Overlay;
static void Require(bool value, const char* why)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
// A French AZERTY-like layout: the unshifted number row types letters, Shift types digits.
static void Azerty(unsigned vk, const NumericInput::Keys& keys, auto&& emit)
{
    const bool shift = keys[0x10], altGr = keys[0x11] && keys[0x12];
    if (vk == 0x32) { emit(altGr ? L'~' : shift ? L'2' : static_cast<wchar_t>(0xE9)); }
    else if (vk == 0x41) { emit(shift ? L'A' : L'q'); }
}
// Presses keys (virtual-key codes) from a clean state and returns the characters typed.
static std::vector<unsigned> Type(std::initializer_list<unsigned> held, bool translate, bool textInput = true)
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    NumericInput input;
    NumericInput::Keys keys{};
    input.Update(io, keys, true);
    io.WantTextInput = textInput;
    for (const auto vk : held) { keys[vk] = true; }
    if (translate) { input.Update(io, keys, true, [](unsigned vk, const auto& state, auto&& emit) { Azerty(vk, state, emit); }); }
    else { input.Update(io, keys, true); }
    std::vector<unsigned> typed;
    for (const auto& event : GImGui->InputEventsQueue) {
        if (event.Type == ImGuiInputEventType_Text) { typed.push_back(event.Text.Char); }
    }
    ImGui::DestroyContext();
    return typed;
}
int main()
{
    Require(Type({0x32}, true) == std::vector<unsigned>{0xE9}, "the unshifted number row types the layout's character");
    Require(Type({0x10, 0x32}, true) == std::vector<unsigned>{'2'}, "Shift and the number row types the layout's digit once");
    Require(Type({0x11, 0x12, 0x32}, true) == std::vector<unsigned>{'~'}, "AltGr and the number row types the layout's character once");
    Require(Type({0x32}, false) == std::vector<unsigned>{'2'}, "without a layout the number row types the digit for numeric fields");
    Require(Type({0x62}, true) == std::vector<unsigned>{'2'}, "the keypad types digits on any layout");
    Require(Type({0x41}, true) == std::vector<unsigned>{'q'}, "letters follow the layout");
    Require(Type({0x11, 0x41}, true).empty(), "Ctrl shortcuts do not type");
    Require(Type({0x32}, true, false).empty(), "nothing is typed while no text field is active");
    std::puts("PASS: overlay text input follows the keyboard layout without duplicate characters");
}
