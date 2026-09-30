#include "ModexMenuScope.h"

#include "HookInstallation.h"
#include "../Shared/ModuleInfo.h"

namespace
{
    inline constexpr auto kIniPath = L"Data\\SKSE\\Plugins\\TheosRenderPipelineImGui.ini";
    inline constexpr std::string_view kMenuName = "ModexGUIMenu";
    // IMenu::PostDisplay; the slot is part of the engine's menu ABI, not Modex's build.
    inline constexpr std::size_t kPostDisplaySlot = 6;

    using PostDisplay = void (*)(RE::IMenu*);
    PostDisplay originalPostDisplay{ nullptr };
    std::atomic_bool installAttempted{ false };
    thread_local bool displayActive{ false };

    void PostDisplayThunk(RE::IMenu* a_menu)
    {
        const bool saved = displayActive;
        displayActive = true;
        originalPostDisplay(a_menu);
        displayActive = saved;
    }

    bool Enabled()
    {
        return ::GetPrivateProfileIntW(L"Adapters", L"EnableAll", 1, kIniPath) != 0 &&
            ::GetPrivateProfileIntW(L"Adapters", L"Modex", 1, kIniPath) != 0;
    }
}

void TheosRenderPipeline::ModexMenuScope::OnMenuOpened()
{
    if (installAttempted.load()) { return; }
    auto* ui = RE::UI::GetSingleton();
    RE::GPtr<RE::IMenu> menu;
    if (ui) { menu = ui->GetMenu(kMenuName); }
    // Retry on a later opening if the instance is not yet published.
    if (!menu || installAttempted.exchange(true)) { return; }
    if (!Enabled()) {
        logger::info("[Modex] preview surface adapter disabled in TheosRenderPipelineImGui.ini");
        return;
    }
    const auto module = ::GetModuleHandleW(L"Modex.dll");
    const auto range = module ? Compatibility::GetModuleRange(module) : std::nullopt;
    if (!range) {
        logger::warn("[Modex] preview surface adapter inactive: Modex.dll is not loaded");
        return;
    }
    std::uintptr_t table{};
    std::uintptr_t current{};
    const auto object = reinterpret_cast<std::uintptr_t>(menu.get());
    if (!HookSafety::Read(object, &table, sizeof(table)) || !range->Contains(table) ||
        !HookSafety::Read(table + kPostDisplaySlot * sizeof(std::uintptr_t), &current, sizeof(current)) ||
        !range->Contains(current)) {
        // Another hook or a different menu class owns this slot; leave it unchanged.
        logger::warn("[Modex] preview surface adapter inactive: PostDisplay is not Modex's own implementation");
        return;
    }
    auto* slot = reinterpret_cast<std::uintptr_t*>(table) + kPostDisplaySlot;
    if (!deviceHookSlots.Install(slot, reinterpret_cast<std::uintptr_t>(&PostDisplayThunk), originalPostDisplay)) {
        logger::error("[Modex] preview surface adapter could not wrap PostDisplay");
        return;
    }
    logger::info("[Modex] preview surface adapter installed at PostDisplay RVA 0x{:X}", current - range->base);
}

bool TheosRenderPipeline::ModexMenuScope::DisplayActive()
{
    return displayActive;
}
