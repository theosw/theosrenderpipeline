#pragma once
#include "HookSafety.h"

namespace TheosRenderPipeline
{
    inline HookSafety::SlotRegistry deviceHookSlots;

    template<class Hook, class Original> void InstallImportHook(std::uintptr_t module, const char* dll, const char* name, Hook hook, Original& original)
    {
        if (!deviceHookSlots.Install(HookSafety::ImportSlot(module, dll, name), reinterpret_cast<std::uintptr_t>(hook), original)) {
            util::report_and_fail(std::format("Could not safely preserve the {} import hook chain. Restart Skyrim; see TheosRenderPipeline.log.", name));
        }
    }

    template<class Hook, class Original> void InstallVTableHook(void* instance, std::size_t index, Hook hook, Original& original)
    {
        std::uintptr_t table{};
        if (!HookSafety::Read(reinterpret_cast<std::uintptr_t>(instance), &table, sizeof(table)) || !table ||
            !deviceHookSlots.Install(reinterpret_cast<std::uintptr_t*>(table) + index,
                reinterpret_cast<std::uintptr_t>(hook), original)) {
            util::report_and_fail(std::format("Could not safely install a renderer vtable hook (slot {}). Restart Skyrim; see TheosRenderPipeline.log.", index));
        }
    }
}
