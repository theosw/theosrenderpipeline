// An interactive window around the production menu: the menu draws into its own
// ImGui context at the chosen game output size, and a sidebar switches
// scenarios, output sizes and renderer state. See README.md.

#include "Localization.h"
#include "OverlayPreview.h"
#include "OverlayUIStyle.h"
#include "ScreenshotFile.h"

#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>

#include <chrono>
#include <cstdio>
#include <filesystem>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline::Overlay;

namespace
{
struct OutputSize
{
    const char* label;
    UINT width, height;
};

constexpr OutputSize kOutputSizes[]{
    {"1280 x 720", 1280, 720},     {"1920 x 1080", 1920, 1080}, {"2560 x 1080", 2560, 1080},
    {"2560 x 1440", 2560, 1440},   {"3440 x 1440", 3440, 1440}, {"3840 x 2160", 3840, 2160},
};

// Win32 virtual keys the menu needs, as ImGui keys.
ImGuiKey MenuKey(WPARAM vk)
{
    if (vk >= '0' && vk <= '9')
        return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
    switch (vk)
    {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
    case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case VK_ADD: return ImGuiKey_KeypadAdd;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_CONTROL: return ImGuiKey_LeftCtrl;
    case VK_SHIFT: return ImGuiKey_LeftShift;
    case VK_MENU: return ImGuiKey_LeftAlt;
    default: return ImGuiKey_None;
    }
}

// Keyboard messages for the menu, queued while the host window pumps messages.
struct KeyEvent
{
    UINT message;
    WPARAM wParam;
};

struct Window
{
    HWND hwnd{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> target;
    UINT resizeWidth{}, resizeHeight{};
    std::vector<KeyEvent> keys;
    bool closed{};
    bool focused{true};
};
Window* g_window{};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam))
        return 1;
    switch (message)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
        if (g_window)
            g_window->keys.push_back({message, wParam});
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (g_window)
            g_window->focused = message == WM_SETFOCUS;
        break;
    case WM_SIZE:
        if (g_window && wParam != SIZE_MINIMIZED)
        {
            g_window->resizeWidth = LOWORD(lParam);
            g_window->resizeHeight = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0; // Alt must not open the window menu.
        break;
    case WM_DESTROY:
        if (g_window)
            g_window->closed = true;
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

bool CreateTarget(Window& window)
{
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(window.swapChain->GetBuffer(0, IID_PPV_ARGS(&back))))
        return false;
    return SUCCEEDED(window.device->CreateRenderTargetView(back.Get(), nullptr, &window.target));
}

bool CreateWindowAndDevice(Window& window, bool warp)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"TRPOverlayPreview";
    ::RegisterClassExW(&windowClass);
    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = (work.right - work.left) * 9 / 10, height = (work.bottom - work.top) * 9 / 10;
    window.hwnd = ::CreateWindowExW(0, windowClass.lpszClassName, L"Theo's Render Pipeline menu preview",
        WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2, width, height, nullptr, nullptr, windowClass.hInstance,
        nullptr);
    if (!window.hwnd)
        return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window.hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT result = E_FAIL;
    for (const auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
    {
        if (warp && type == D3D_DRIVER_TYPE_HARDWARE)
            continue;
        result = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc,
            &window.swapChain, &window.device, nullptr, &window.context);
        if (SUCCEEDED(result))
            break;
    }
    if (FAILED(result) || !CreateTarget(window))
        return false;
    ::ShowWindow(window.hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(window.hwnd);
    return true;
}

// The game output the menu draws into, and the view of it the host shows.
struct MenuTarget
{
    UINT width{}, height{};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> resource;

    bool Create(ID3D11Device* device, UINT w, UINT h)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texture.Reset();
        target.Reset();
        resource.Reset();
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) ||
            FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &target)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &resource)))
            return false;
        width = w;
        height = h;
        return true;
    }
};

enum class NRState { Off, Active, Waiting, Failed };

NRState CurrentNRState(const OverlayUI::FrameView& view)
{
    if (view.neuralState.failed)
        return NRState::Failed;
    if (!view.neuralApplied.enabled)
        return NRState::Off;
    return view.neuralState.active ? NRState::Active : NRState::Waiting;
}

void SetNRState(OverlayUI::FrameView& view, NRState state)
{
    auto& applied = view.neuralApplied;
    auto& neural = view.neuralState;
    applied.enabled = state != NRState::Off;
    if (applied.enabled && applied.passes < 1)
        applied.passes = 1;
    neural.active = state == NRState::Active;
    neural.failed = state == NRState::Failed;
    neural.effectivePasses = neural.active ? applied.passes : 0;
    neural.status = state == NRState::Active    ? "NR active"
                    : state == NRState::Waiting ? "NR waiting for the first world frame"
                    : state == NRState::Failed  ? "NR failed: the runtime refused the model (sample error)"
                                                : "NR off";
}

class LivePreview
{
public:
    LivePreview(Window& window, const LivePreviewOptions& options)
        : window_(window), options_(options), scenarios_(PreviewScenarios())
    {
        for (std::size_t i = 0; i < scenarios_.size(); ++i)
        {
            if (scenarios_[i].name == options.scenario)
                scenarioIndex_ = static_cast<int>(i);
        }
        size_ = {options.width, options.height};
        for (std::size_t i = 0; i < std::size(kOutputSizes); ++i)
        {
            if (kOutputSizes[i].width == size_.width && kOutputSizes[i].height == size_.height)
                sizeIndex_ = static_cast<int>(i);
        }
    }

    bool Init()
    {
        dpi_ = ImGui_ImplWin32_GetDpiScaleForHwnd(window_.hwnd);
        // As OverlayUI::Init does in game, before either atlas is built.
        TheosRenderPipeline::Localization::Activate(options_.language);
        host_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(host_);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(dpi_);
        BuildRendererFont(dpi_);
        if (!ImGui_ImplWin32_Init(window_.hwnd) || !ImGui_ImplDX11_Init(window_.device.Get(), window_.context.Get()))
            return false;

        menu_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(menu_);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigWindowsResizeFromEdges = true;
        ApplyRendererStyle(1.0f);
        BuildRendererFont(1.0f);
        if (!ImGui_ImplDX11_Init(window_.device.Get(), window_.context.Get()))
            return false;
        ImGui::SetCurrentContext(host_);
        return ResizeOutput(size_.width, size_.height) && LoadScenario();
    }

    ~LivePreview()
    {
        preview_.reset();
        if (menu_)
        {
            ImGui::SetCurrentContext(menu_);
            ImGui_ImplDX11_Shutdown();
            ImGui::DestroyContext(menu_);
        }
        if (host_)
        {
            ImGui::SetCurrentContext(host_);
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(host_);
        }
    }

    void Frame()
    {
        // Before the host frame, which then draws the new output texture.
        ApplyPendingChanges();
        ImGui::SetCurrentContext(host_);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawSidebar();
        DrawViewport();
        RunScript();
        ImGui::Render();

        DrawMenu();

        ImGui::SetCurrentContext(host_);
        const float clear[4]{0.06f, 0.06f, 0.07f, 1.0f};
        window_.context->OMSetRenderTargets(1, window_.target.GetAddressOf(), nullptr);
        window_.context->ClearRenderTargetView(window_.target.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

private:
    bool LoadScenario()
    {
        // The menu keeps its window and tab state; the scenario starts afresh.
        preview_.reset();
        scenario_ = AtOutputSize(scenarios_[scenarioIndex_], size_.width, size_.height);
        // A reload keeps the window where it was dragged.
        const auto layout = lastLayout_ ? *lastLayout_ : options_.layout.value_or(Layout{});
        ImGui::SetCurrentContext(menu_);
        // The starting tab applies to the first load; reloads keep the user's tab.
        preview_ = std::make_unique<OverlayPreview>(scenario_, layout, loaded_ ? SettingsPage::None : options_.page);
        loaded_ = true;
        preview_->SetLive(true);
        if (labOverride_)
            preview_->SetLabMode(*labOverride_);
        ImGui::SetCurrentContext(host_);
        return true;
    }

    bool ResizeOutput(UINT width, UINT height)
    {
        if (!target_.Create(window_.device.Get(), width, height))
            return false;
        size_ = {width, height};
        return true;
    }

    void DrawSidebar()
    {
        const auto display = ImGui::GetIO().DisplaySize;
        sidebarWidth_ = 330.0f * dpi_;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(sidebarWidth_, display.y));
        ImGui::Begin("Preview", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PushItemWidth(-FLT_MIN);

        ImGui::SeparatorText("Scenario");
        if (ImGui::BeginCombo("##scenario", scenarios_[scenarioIndex_].name.c_str()))
        {
            for (std::size_t i = 0; i < scenarios_.size(); ++i)
            {
                if (ImGui::Selectable(scenarios_[i].name.c_str(), static_cast<int>(i) == scenarioIndex_))
                {
                    scenarioIndex_ = static_cast<int>(i);
                    reloadScenario_ = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", scenarios_[i].summary.c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::TextWrapped("%s", scenarios_[scenarioIndex_].summary.c_str());
        if (ImGui::Button("Reset scenario"))
            reloadScenario_ = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restores the scenario's renderer state and settings. Apply in the menu keeps "
                              "edits until then.");

        ImGui::SeparatorText("Game output");
        if (ImGui::BeginCombo("##output", sizeIndex_ >= 0 ? kOutputSizes[sizeIndex_].label : "Custom"))
        {
            for (std::size_t i = 0; i < std::size(kOutputSizes); ++i)
            {
                if (ImGui::Selectable(kOutputSizes[i].label, static_cast<int>(i) == sizeIndex_))
                {
                    sizeIndex_ = static_cast<int>(i);
                    pendingSize_ = {kOutputSizes[i].width, kOutputSizes[i].height};
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Fit to window", &fit_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off shows the output pixel for pixel; drag the scroll bars to move around.");
        ImGui::TextDisabled("Shown at %.0f%%", viewScale_ * 100.0f);

        ImGui::SeparatorText("Renderer state");
        auto& view = scenario_.view;
        bool lab = preview_->LabMode();
        if (ImGui::Checkbox("Lab mode", &lab))
        {
            preview_->SetLabMode(lab);
            labOverride_ = lab;
        }
        ImGui::Checkbox("Community Shaders", &view.communityShaders);
        ImGui::Checkbox("NVIDIA GPU", &view.nvidiaAdapter);
        bool runtime = preview_->RuntimePresent();
        if (ImGui::Checkbox("NR runtime installed", &runtime))
            preview_->SetRuntimePresent(runtime);
        const char* nrStates[]{"Off", "Active", "Waiting", "Failed"};
        int nr = static_cast<int>(CurrentNRState(view));
        ImGui::TextUnformatted("Neural Rendering");
        if (ImGui::Combo("##nr", &nr, nrStates, IM_ARRAYSIZE(nrStates)))
            SetNRState(view, static_cast<NRState>(nr));
        ImGui::Checkbox("Frame generation running", &view.generationActive);
        int provider = static_cast<int>(view.provider);
        const char* providers[]{"NVIDIA DLSS-G", "Intel XeFG"};
        ImGui::TextUnformatted("Generation provider");
        if (ImGui::Combo("##provider", &provider, providers, IM_ARRAYSIZE(providers)))
            view.provider = view.requestedProvider = static_cast<TheosRenderPipeline::FrameGenerationProvider>(provider);
        ImGui::TextDisabled("These change what the menu reads; they do\nnot follow Apply as the game would.");

        ImGui::SeparatorText("Capture");
        if (ImGui::Button("Save PNG of the menu"))
            saveRequested_ = true;
        if (!saved_.empty())
            ImGui::TextWrapped("%s", saved_.c_str());

        ImGui::PopItemWidth();
        ImGui::End();
    }

    void DrawViewport()
    {
        const auto display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(sidebarWidth_, 0));
        ImGui::SetNextWindowSize(ImVec2((std::max)(1.0f, display.x - sidebarWidth_), display.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##viewport", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoScrollWithMouse | (fit_ ? 0 : ImGuiWindowFlags_HorizontalScrollbar));
        ImGui::PopStyleVar();
        const auto available = ImGui::GetContentRegionAvail();
        const float width = static_cast<float>(target_.width), height = static_cast<float>(target_.height);
        viewScale_ = fit_ ? (std::min)(1.0f, (std::min)(available.x / width, available.y / height)) : 1.0f;
        const ImVec2 shown(width * viewScale_, height * viewScale_);
        if (fit_)
            ImGui::SetCursorPos(ImVec2((available.x - shown.x) * 0.5f, (available.y - shown.y) * 0.5f));
        imageOrigin_ = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(target_.resource.Get()), shown);
        imageHovered_ = ImGui::IsItemHovered();
        if (imageHovered_)
            ImGui::SetMouseCursor(menuCursor_);
        ImGui::End();

        // The menu keeps the mouse while a button pressed over it stays down.
        auto& io = ImGui::GetIO();
        const bool anyDown = io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
        if (imageHovered_ && (io.MouseClicked[0] || io.MouseClicked[1] || io.MouseClicked[2]))
        {
            mouseCaptured_ = true;
            keyboardToMenu_ = true;
        }
        else if (!imageHovered_ && (io.MouseClicked[0] || io.MouseClicked[1]))
        {
            keyboardToMenu_ = false;
        }
        if (!anyDown && !imageHovered_)
            mouseCaptured_ = false;
        forwardMouse_ = imageHovered_ || mouseCaptured_;
        mousePos_ = ImVec2((io.MousePos.x - imageOrigin_.x) / viewScale_, (io.MousePos.y - imageOrigin_.y) / viewScale_);
        for (int i = 0; i < 3; ++i)
            mouseDown_[i] = io.MouseDown[i] && forwardMouse_;
        wheel_ = imageHovered_ ? ImVec2(io.MouseWheelH, io.MouseWheel) : ImVec2(0, 0);
    }

    // Scripted runs drive the menu's input directly, so the real cursor cannot
    // interfere, and save once the steps are done.
    void RunScript()
    {
        if (!Scripted() || saveRequested_)
            return;
        forwardMouse_ = keyboardToMenu_ = true;
        for (auto& down : mouseDown_)
            down = false;
        wheel_ = ImVec2(0, 0);
        // The mouse stays at the last step for the saved frame, so a hover shows.
        mousePos_ = scriptMouse_;
        // As PNG mode, let the first frames settle before any step or save: the menu
        // window and new tab headers only take their place after a frame or two.
        if (settleFrames_ < 4)
        {
            ++settleFrames_;
            return;
        }
        if (step_ >= options_.script.size())
        {
            saveRequested_ = true;
            return;
        }
        const auto& step = options_.script[step_];
        mousePos_ = scriptMouse_ = ImVec2(menuRect_.Min.x + step.x, menuRect_.Min.y + step.y);
        if (!step.text.empty())
        {
            if (stepFrame_ == 0)
                typing_ = step.text;
            if (++stepFrame_ >= 4 && typing_.empty() && !enterDown_)
            {
                ++step_;
                stepFrame_ = 0;
            }
            return;
        }
        mouseDown_[0] = step.click && stepFrame_ >= 2 && stepFrame_ < 4;
        if (++stepFrame_ >= (step.click ? 10 : 60))
        {
            ++step_;
            stepFrame_ = 0;
        }
    }

    bool Scripted() const { return !options_.script.empty() || !options_.save.empty(); }

public:
    bool Finished() const { return finished_; }
    bool SaveFailed() const { return saveFailed_; }

private:
    void ApplyPendingChanges()
    {
        if (pendingSize_.width)
        {
            ResizeOutput(pendingSize_.width, pendingSize_.height);
            pendingSize_ = {};
            reloadScenario_ = true;
        }
        if (reloadScenario_)
        {
            lastLayout_ = preview_->CurrentLayout();
            LoadScenario();
            reloadScenario_ = false;
        }
    }

    void ForwardInput()
    {
        auto& io = ImGui::GetIO();
        // A scripted run draws the in-game software cursor, as the game does, so a
        // saved PNG shows it; interactive runs keep the Windows cursor.
        io.MouseDrawCursor = Scripted() && forwardMouse_;
        if (forwardMouse_)
            io.AddMousePosEvent(mousePos_.x, mousePos_.y);
        else
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        for (int i = 0; i < 3; ++i)
        {
            if (mouseDown_[i] != menuMouseDown_[i])
            {
                io.AddMouseButtonEvent(i, mouseDown_[i]);
                menuMouseDown_[i] = mouseDown_[i];
            }
        }
        if (wheel_.x != 0 || wheel_.y != 0)
            io.AddMouseWheelEvent(wheel_.x, wheel_.y);
        // Scripted typing: one character or key per frame.
        if (enterDown_)
        {
            io.AddKeyEvent(ImGuiKey_Enter, false);
            enterDown_ = false;
        }
        else if (!typing_.empty())
        {
            const char c = typing_.front();
            typing_.erase(0, 1);
            if (c == '\n')
            {
                io.AddKeyEvent(ImGuiKey_Enter, true);
                enterDown_ = true;
            }
            else
                io.AddInputCharacter(static_cast<unsigned>(c));
        }
        for (const auto& key : window_.keys)
        {
            if (!keyboardToMenu_)
                break;
            if (key.message == WM_CHAR)
            {
                io.AddInputCharacterUTF16(static_cast<ImWchar16>(key.wParam));
                continue;
            }
            const bool down = key.message == WM_KEYDOWN || key.message == WM_SYSKEYDOWN;
            if (key.wParam == VK_CONTROL)
                io.AddKeyEvent(ImGuiMod_Ctrl, down);
            else if (key.wParam == VK_SHIFT)
                io.AddKeyEvent(ImGuiMod_Shift, down);
            else if (key.wParam == VK_MENU)
                io.AddKeyEvent(ImGuiMod_Alt, down);
            if (const auto mapped = MenuKey(key.wParam); mapped != ImGuiKey_None)
                io.AddKeyEvent(mapped, down);
        }
        window_.keys.clear();
        // Scripted runs type without the window having focus.
        const bool focused = keyboardToMenu_ && (window_.focused || Scripted());
        if (focused != menuFocused_)
        {
            // Losing focus, to the sidebar or another application, releases any keys the menu still holds.
            io.AddFocusEvent(focused);
            menuFocused_ = focused;
        }
    }

    void DrawMenu()
    {
        ImGui::SetCurrentContext(menu_);
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(target_.width), static_cast<float>(target_.height));
        const auto now = std::chrono::steady_clock::now();
        io.DeltaTime = lastFrame_.time_since_epoch().count()
                           ? (std::max)(1e-4f, std::chrono::duration<float>(now - lastFrame_).count())
                           : 1.0f / 60.0f;
        lastFrame_ = now;
        ForwardInput();
        preview_->UpdateScale();
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        preview_->BuildFrame();
        menuCursor_ = ImGui::GetMouseCursor();
        if (const auto* drawn = ImGui::FindWindowByName(Plugin::DISPLAY_NAME.data()))
            menuRect_ = drawn->Rect();
        ImGui::Render();

        // A neutral backdrop in place of the game frame.
        const float backdrop[4]{0.16f, 0.17f, 0.18f, 1.0f};
        window_.context->ClearRenderTargetView(target_.target.Get(), backdrop);
        window_.context->OMSetRenderTargets(1, target_.target.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(target_.width), static_cast<float>(target_.height), 0, 1};
        window_.context->RSSetViewports(1, &viewport);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (saveRequested_)
        {
            saveRequested_ = false;
            saveFailed_ = !Save(Scripted() && options_.full ? ImRect() : menuRect_);
            finished_ = Scripted();
        }
    }

    bool Save(const ImRect& rect)
    {
        const auto image = ReadPreviewTexture(window_.device.Get(), window_.context.Get(), target_.texture.Get(), rect);
        const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::error_code error;
        const auto path = std::filesystem::absolute(!options_.save.empty() ? options_.save
            : options_.out / std::format("live-{}-{}x{}-{}.png", scenarios_[scenarioIndex_].name, target_.width,
                               target_.height, stamp));
        std::filesystem::create_directories(path.parent_path(), error);
        const auto result = image ? TheosRenderPipeline::ScreenshotFile::Replace(path, image->width, image->height,
                                        image->bgr, 0)
                                  : E_FAIL;
        saved_ = SUCCEEDED(result) ? "Saved " + path.string()
                                   : std::format("Could not save the PNG (0x{:08X})", static_cast<unsigned>(result));
        std::printf("%s\n", saved_.c_str());
        return SUCCEEDED(result);
    }

    Window& window_;
    LivePreviewOptions options_;
    std::vector<PreviewScenario> scenarios_;
    PreviewScenario scenario_;
    std::unique_ptr<OverlayPreview> preview_;
    std::optional<Layout> lastLayout_;
    std::optional<bool> labOverride_;
    ImGuiContext* host_{};
    ImGuiContext* menu_{};
    MenuTarget target_;
    float dpi_{1.0f};
    int scenarioIndex_{};
    int sizeIndex_{-1};
    struct
    {
        UINT width{}, height{};
    } size_, pendingSize_;
    bool fit_{true};
    float viewScale_{1.0f};
    float sidebarWidth_{};
    ImVec2 imageOrigin_{};
    bool imageHovered_{}, mouseCaptured_{}, forwardMouse_{}, keyboardToMenu_{}, menuFocused_{true};
    ImVec2 mousePos_{}, wheel_{};
    bool mouseDown_[3]{}, menuMouseDown_[3]{};
    ImGuiMouseCursor menuCursor_{ImGuiMouseCursor_Arrow};
    std::chrono::steady_clock::time_point lastFrame_{};
    bool reloadScenario_{}, saveRequested_{}, finished_{}, saveFailed_{}, loaded_{};
    int settleFrames_{};
    ImRect menuRect_;
    ImVec2 scriptMouse_{-FLT_MAX, -FLT_MAX};
    std::string typing_;
    bool enterDown_{};
    std::size_t step_{};
    int stepFrame_{};
    std::string saved_;
};
} // namespace

int RunLivePreview(const LivePreviewOptions& options)
{
    Window window;
    g_window = &window;
    if (!CreateWindowAndDevice(window, options.warp))
    {
        std::fputs("Could not create the preview window.\n", stderr);
        return 1;
    }
    int result = 0;
    {
        LivePreview preview(window, options);
        if (!preview.Init())
        {
            std::fputs("Could not initialize the preview.\n", stderr);
            result = 1;
        }
        while (!result && !window.closed)
        {
            MSG message;
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            if (window.closed)
                break;
            if (::IsIconic(window.hwnd))
            {
                ::Sleep(50);
                continue;
            }
            if (window.resizeWidth && window.resizeHeight)
            {
                window.target.Reset();
                window.swapChain->ResizeBuffers(0, window.resizeWidth, window.resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
                window.resizeWidth = window.resizeHeight = 0;
                CreateTarget(window);
            }
            preview.Frame();
            window.swapChain->Present(1, 0);
            if (preview.Finished())
            {
                // A scripted run fails when its image could not be saved.
                result = preview.SaveFailed() ? 1 : 0;
                break;
            }
        }
    }
    if (window.hwnd)
        ::DestroyWindow(window.hwnd);
    g_window = nullptr;
    return result;
}
