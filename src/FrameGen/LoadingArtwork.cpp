#include <PCH.h>
#include "LoadingArtwork.h"
#include "LoadingArtworkPolicy.h"
#include "LoadingScreenState.h"
#include "RenderPipeline.h"
#include <atomic>
#include <cstring>
#include <intrin.h>

namespace TheosRenderPipeline::LoadingArtwork
{
    namespace
    {
        LoadingWorldReadiness worldReadiness;
        std::atomic_bool worldReady{};

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
                    REL::Relocate(kSECallers, kAECallers),
                    RenderPipeline::GetSingleton()->mRequestLoadingArtwork.load(std::memory_order_relaxed),
                    ready, gameCaller, callerRVA, show, suppress, immediate);
                if (effective != suppress) {
                    logger::info("[LoadingArtwork] requested artwork for queued transition callerRVA=0x{:X}", callerRVA);
                }
                return original(show, location, effective, immediate);
            }
            static inline REL::Relocation<decltype(thunk)> original;
        };
    }

    void Boundary(std::uint64_t frame, bool world, bool mainMenu, bool loadingMenu) noexcept
    {
        worldReady.store(worldReadiness.Boundary(frame, world, mainMenu, loadingMenu), std::memory_order_relaxed);
    }

    void ResetAfterRetirement() noexcept
    {
        worldReady.store(false, std::memory_order_relaxed);
        worldReadiness.ResetAfterRetirement();
    }

    void Install()
    {
        const auto target = REL::RelocationID(13214, 13363).address();
        constexpr std::array<std::uint8_t, 19> prologue{
            0x40,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x40,0x48,0xC7,0x44,0x24,0x38,0xFE,0xFF,0xFF,0xFF};
        if (std::memcmp(reinterpret_cast<const void*>(target), prologue.data(), prologue.size()) != 0) {
            util::report_and_fail("Loading artwork hook does not match the engine transition sender.");
        }
        Transition::original = Detours::X64::DetourFunction(target, reinterpret_cast<std::uintptr_t>(&Transition::thunk));
        if (!Transition::original.address()) {
            util::report_and_fail("Could not install the loading artwork transition hook.");
        }
        logger::info("[LoadingArtwork] transition hook installed; requests follow the menu setting after settled gameplay");
    }
}
