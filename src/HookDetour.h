#pragma once
#include "HookSafety.h"
#include <detours/Detours.h>

namespace TheosRenderPipeline::HookSafety
{
    inline std::uintptr_t InstallEntryDetour(std::uintptr_t site, std::uintptr_t replacement)
    {
        std::array<std::uint8_t, 6> bytes{};
        if (!Executable(site) || !Executable(replacement) || site == replacement ||
            !Read(site, bytes.data(), bytes.size())) { return 0; }
        const bool priorJump = bytes[0] == 0xE9 || (bytes[0] == 0xFF && bytes[1] == 0x25);
        const auto previous = priorJump ? EntryJumpTarget(site) : 0;
        if (priorJump && (!previous || previous == replacement)) { return 0; }
        // Nukem Detours copies displaced instructions without relocating them.
        // Its copied E9/FF25 is not callable from the new trampoline. Forward
        // directly to the validated predecessor; ordinary prologues retain the
        // existing trampoline path. Callers preflight their engine contracts.
        const auto trampoline = Detours::X64::DetourFunction(site, replacement);
        return trampoline ? (previous ? previous : trampoline) : 0;
    }
}
