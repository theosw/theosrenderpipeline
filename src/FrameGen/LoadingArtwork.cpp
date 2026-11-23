#include <PCH.h>
#include "LoadingArtwork.h"
#include "LoadingArtworkPolicy.h"
#include "LoadingScreenState.h"
#include "RenderPipeline.h"
#include "SkyrimRuntime.h"
#include "../SessionDegradationLog.h"
#include <atomic>
#include <cstring>
#include <intrin.h>

namespace TheosRenderPipeline::LoadingArtwork
{
    namespace
    {
        LoadingWorldReadiness worldReadiness;
        std::atomic_bool worldReady{};
        std::atomic_bool forcedTransition{};

        struct Transition
        {
            static __declspec(noinline) std::uintptr_t thunk(std::uint8_t show, void* location,
                std::uint8_t suppress, std::uint8_t immediate)
            {
                const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
                HMODULE module{};
                const bool gameCaller = GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(caller), &module) &&
                    reinterpret_cast<std::uintptr_t>(module) == REL::Module::get().base();
                const auto callerRVA = gameCaller ? caller - REL::Module::get().base() : 0;
                const bool ready = worldReady.load(std::memory_order_relaxed);
                const auto effective = Suppression(
                    callers,
                    RenderPipeline::GetSingleton()->mRequestLoadingArtwork.load(std::memory_order_relaxed),
                    ready, gameCaller, callerRVA, show, suppress, immediate);
                if (effective != suppress) {
                    forcedTransition.store(true, std::memory_order_release);
                    logger::info("[LoadingArtwork] requested artwork for queued transition callerRVA=0x{:X}", callerRVA);
                }
                return original(show, location, effective, immediate);
            }
            static inline REL::Relocation<decltype(thunk)> original;
            static inline TransitionCallers callers{};
        };
    }

    void Boundary(std::uint64_t frame, bool world, bool mainMenu, bool loadingMenu) noexcept
    {
        worldReady.store(worldReadiness.Boundary(frame, world, mainMenu, loadingMenu), std::memory_order_relaxed);
    }

    bool TakeForcedTransition() noexcept { return forcedTransition.exchange(false, std::memory_order_acq_rel); }

    void ResetAfterRetirement() noexcept
    {
        forcedTransition.store(false, std::memory_order_relaxed);
        worldReady.store(false, std::memory_order_relaxed);
        worldReadiness.ResetAfterRetirement();
    }

    bool Install()
    {
        // Loading artwork is optional: when the transition sender is not the code this
        // hook was written for (another mod patched it, or an unknown runtime), nothing
        // is written and the feature is off for the session.
        const auto unavailable = [](const char* reason) {
            logger::warn("[LoadingArtwork] {}; loading artwork requests are off this session", reason);
            Degradation::Note(Degradation::Reason::LoadingArtworkUnavailable, reason);
            return false;
        };
        const auto* profile = SkyrimRuntime::Find(REL::Module::get().version());
        const auto target = profile ? REL::RelocationID(13214, 13363).address() : 0;
        const bool matches = target && std::memcmp(reinterpret_cast<const void*>(target), kTransitionPrologue.data(), kTransitionPrologue.size()) == 0;
        if (const auto* refusal = InstallRefusal(profile != nullptr, matches)) { return unavailable(refusal); }
        // Select before publishing the detour. Other AE versions must not inherit
        // 1.6.1170's raw return addresses just because they share relocation IDs.
        Transition::callers = profile->loadingArtwork;
        Transition::original = Detours::X64::DetourFunction(target, reinterpret_cast<std::uintptr_t>(&Transition::thunk));
        // The detour library returns 0 before it writes anything.
        if (!Transition::original.address()) { return unavailable("Could not install the loading artwork transition hook"); }
        logger::info("[LoadingArtwork] transition hook installed; requests follow the menu setting after settled gameplay");
        return true;
    }
}
