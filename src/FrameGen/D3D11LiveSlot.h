#pragma once

#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace TheosRenderPipeline
{
    // D3D11 keeps an immediate context's function table inside the context
    // object and rewrites it from the context-state template whenever state is
    // swapped, including by our own SwapDeviceContextState isolation. Ordinary
    // runtime operations such as Flush can also rewrite it. A hook written into
    // that table is therefore lost until reinstalled. Reinstall it
    // before the producer calls that must be observed; the displaced entry is
    // the function to forward to.
    class D3D11LiveSlot
    {
    public:
        // True when the slot now reaches `hook`. Installs counts actual writes.
        bool Ensure(void* instance, std::size_t index, std::uintptr_t hook)
        {
            if (!instance || !hook) { return false; }
            auto* slot = *reinterpret_cast<std::uintptr_t* const*>(instance) + index;
            const auto current = *slot;
            if (current == hook) { return true; }
            if (!current) { return false; }
            // Publish the forward target before the hook can be entered.
            original_.store(current, std::memory_order_release);
            DWORD previous{};
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous)) { return false; }
            const auto exchanged = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                reinterpret_cast<void*>(hook), reinterpret_cast<void*>(current));
            DWORD ignored{};
            VirtualProtect(slot, sizeof(*slot), previous, &ignored);
            if (exchanged != reinterpret_cast<void*>(current)) { return false; }
            installs_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        std::uintptr_t Original() const { return original_.load(std::memory_order_acquire); }
        std::uint64_t Installs() const { return installs_.load(std::memory_order_relaxed); }

    private:
        std::atomic<std::uintptr_t> original_{};
        std::atomic<std::uint64_t> installs_{};
    };
}
