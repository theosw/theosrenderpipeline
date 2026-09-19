#include "OverlayNumericInput.h"
#include <imgui_internal.h>
#include <cstdio>
#include <cstdlib>

namespace
{
	void Require(bool ok, const char* message)
	{
		if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
	}
}

int main()
{
	ImGui::CreateContext();
	auto& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = { 800, 600 };
	io.DeltaTime = 1.0f / 60;
	io.ConfigInputTrickleEventQueue = false;
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	TheosRenderPipeline::Overlay::NumericInput input;
	TheosRenderPipeline::Overlay::NumericInput::Keys keys{};
	int value = 0;
	ImVec2 fieldMin{}, fieldMax{};
	bool focus = true;
    bool capturingHotkey = false;
    bool lostApplicationFocus = false;
	auto frame = [&] {
		input.Update(io, keys, focus, capturingHotkey);
		ImGui::NewFrame();
        lostApplicationFocus = io.AppFocusLost;
		ImGui::SetNextWindowPos({ 20, 20 });
		ImGui::SetNextWindowSize({ 480, 240 });
		ImGui::Begin("FPS test", nullptr, ImGuiWindowFlags_NoSavedSettings);
		TheosRenderPipeline::Overlay::FPSInput("##fps", value);
		fieldMin = ImGui::GetItemRectMin(); fieldMax = ImGui::GetItemRectMax();
		ImGui::End();
		ImGui::Render();
	};
	auto tap = [&](unsigned vk) { keys[vk] = true; frame(); keys[vk] = false; frame(); };
	auto selectAll = [&] {
		keys[0x11] = true; frame(); tap('A'); keys[0x11] = false; frame();
	};
	auto clickField = [&] {
		io.AddMousePosEvent(fieldMin.x + 20, (fieldMin.y + fieldMax.y) / 2);
		io.AddMouseButtonEvent(0, true); frame();
		io.AddMouseButtonEvent(0, false); frame(); frame();
	};
	frame(); frame();
	Require(fieldMax.x - fieldMin.x > 400, "textbox uses available width");
	clickField();
	Require(io.WantTextInput, "click activates numeric textbox");
	tap('2'); tap('2');
	Require(value == 22, "type two digits without clicking stepper");
	selectAll(); tap('3'); tap('0');
	Require(value == 30, "Ctrl+A replaces the existing cap");
	selectAll(); tap('1'); Require(value == 1, "first digit retained");
	tap('2'); Require(value == 12, "second digit retained");
	tap('0'); Require(value == 120, "three-digit direct entry");
	selectAll(); tap('3'); tap('0');
	tap(0x08);
	Require(value == 3, "Backspace edits the value");
	tap(0x65);
	Require(value == 35, "numpad digit entry");
	tap(0x24); tap(0x2E);
	Require(value == 5, "Home and Delete editing");
	selectAll(); tap('6'); tap('0'); tap(0x0D); frame();
	Require(value == 60 && !io.WantTextInput, "Enter commits and ends text editing");
	io.GetClipboardTextFn = [](void*) { return "120"; };
	io.SetClipboardTextFn = [](void*, const char*) {};
	clickField(); selectAll(); keys[0x11] = true; frame(); tap('V'); keys[0x11] = false; frame();
	Require(value == 120, "paste edits the cap through ImGui without OS clipboard access");
    // Binding capture suspends numeric input without reporting application
    // focus loss. A held selected key
    // stays blocked until released when capture ends.
    capturingHotkey = true;
    keys['8'] = true;
    frame();
    Require(!lostApplicationFocus && value == 120, "capture preserves UI focus and numeric value");
    capturingHotkey = false;
    frame(); frame();
    Require(value == 120, "held capture key does not type after capture");
    keys['8'] = false;
    frame();
    capturingHotkey = true;
    focus = false;
    frame();
    Require(lostApplicationFocus, "real focus loss still reaches ImGui during capture");
    capturingHotkey = false;
    focus = true;
    frame();
	tap(0x0D); frame(); clickField(); tap('9'); tap(0x1B); frame();
	Require(value == 120 && !io.WantTextInput, "Escape cancels an uncommitted text edit");
	tap('9');
	Require(value == 120, "no text injected outside a text field");
	clickField(); selectAll(); tap('3');
	focus = false; keys['8'] = true; frame();
	Require(!io.KeyCtrl, "focus loss releases modifiers");
	focus = true; frame();
	Require(value == 3, "held key on focus return does not type");
	keys['8'] = false; frame();
	clickField(); selectAll(); tap('9'); tap('0');
	Require(value == 90, "typing resumes after focus return");
	keys[0x11] = true; frame();
	focus = false; frame();
	Require(!io.KeyCtrl && !ImGui::IsKeyDown(ImGuiKey_Backspace), "close/focus loss leaves no stuck keys");
	focus = true; frame();
	clickField(); selectAll(); tap('2'); tap('4'); tap('0');
	Require(value == 240 && io.WantTextInput, "direct 240 entry owns text focus");
	input.Update(io, {}, false);
	ImGui::ClearActiveID();
	ImGui::GetCurrentContext()->WantTextInputNextFrame = 0;
	io.ClearInputKeys();
	io.ClearEventsQueue();
	io.WantTextInput = false;
	Require(!io.WantTextInput && !ImGui::IsAnyItemActive(), "overlay close clears the active numeric field immediately");
	frame();
	Require(!io.WantTextInput, "overlay reopen does not inherit stale text capture");
	ImGui::DestroyContext();
	std::puts("Overlay numeric input: PASS (typing, selection, numpad, editing, focus)");
}
