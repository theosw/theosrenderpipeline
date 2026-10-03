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
static int Count(ImGuiInputEventType type)
{
    int count = 0;
    for (const auto& event : GImGui->InputEventsQueue) { count += event.Type == type; }
    return count;
}
static int FocusLost()
{
    int count = 0;
    for (const auto& event : GImGui->InputEventsQueue) {
        count += event.Type == ImGuiInputEventType_Focus && !event.AppFocused.Focused;
    }
    return count;
}
// Picking a menu key suspends editing without reporting a window focus change,
// and the picked key cannot type once editing resumes while it is still held.
static void SuspendForKeyPicker()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    NumericInput input;
    NumericInput::Keys keys{};
    input.Update(io, keys, true);
    io.WantTextInput = true;
    keys[0x32] = true;
    input.Suspend(io, true);
    input.Update(io, keys, true);
    Require(FocusLost() == 0, "suspending and resuming do not report focus loss");
    Require(Count(ImGuiInputEventType_Text) == 0, "a key held through the picker does not type");
    keys[0x32] = false;
    input.Update(io, keys, true);
    keys[0x32] = true;
    input.Update(io, keys, true);
    Require(Count(ImGuiInputEventType_Text) == 1, "a fresh press types after the picker");
    input.Suspend(io, false);
    Require(FocusLost() == 1, "real focus loss is still reported while suspended");
    ImGui::DestroyContext();
}
int main()
{
    SuspendForKeyPicker();
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
