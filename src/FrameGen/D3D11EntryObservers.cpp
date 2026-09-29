#include "D3D11EntryObservers.h"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <mutex>
#include <utility>

namespace TheosRenderPipeline::D3D11EntryObservers
{
    namespace
    {
        constexpr std::size_t Capacity = 16;
        std::mutex installMutex;
        thread_local unsigned depth{};
        struct Scope { Scope() { ++depth; } ~Scope() { --depth; } };

        template<class Function, class Observer> struct Entry
        {
            void* target{};
            std::atomic<Function> original{};
            Observer observer{};
            bool enabled{};
        };
        std::array<Entry<Dispatch, DispatchObserver>, Capacity> dispatchEntries;
        std::array<Entry<Copy, CopyObserver>, Capacity> copyEntries;
        std::atomic<std::size_t> dispatchCount{}, copyCount{};

        template<std::size_t Index>
        void STDMETHODCALLTYPE DispatchHook(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
        {
            auto& entry = dispatchEntries[Index];
            const auto original = entry.original.load(std::memory_order_acquire);
            const bool observe = depth == 0;
            Scope scope;
            if (observe) { entry.observer(context, x, y, z, original); }
            original(context, x, y, z);
        }
        template<std::size_t Index>
        void STDMETHODCALLTYPE CopyHook(ID3D11DeviceContext* context, ID3D11Resource* destination, ID3D11Resource* source)
        {
            auto& entry = copyEntries[Index];
            const auto original = entry.original.load(std::memory_order_acquire);
            const bool observe = depth == 0;
            Scope scope;
            if (observe) { entry.observer(context, destination, source); }
            original(context, destination, source);
        }
        template<std::size_t... Indices> auto DispatchHooks(std::index_sequence<Indices...>)
        { return std::array<Dispatch, Capacity>{&DispatchHook<Indices>...}; }
        template<std::size_t... Indices> auto CopyHooks(std::index_sequence<Indices...>)
        { return std::array<Copy, Capacity>{&CopyHook<Indices>...}; }

        template<class Function, class Observer>
        bool Install(void* target, Observer observer, std::array<Entry<Function, Observer>, Capacity>& entries,
            const std::array<Function, Capacity>& hooks, std::atomic<std::size_t>& count)
        {
            if (!target || !observer) { return false; }
            for (std::size_t i = 0; i < entries.size(); ++i) {
                auto& entry = entries[i];
                if (entry.target && entry.target != target) { continue; }
                if (entry.target && entry.observer != observer) { return false; }
                if (!entry.target) {
                    void* original{};
                    if (MH_CreateHook(target, reinterpret_cast<void*>(hooks[i]), &original) != MH_OK) { return false; }
                    entry.target = target;
                    entry.observer = observer;
                    // Publish before enabling: another thread can enter the hook
                    // as soon as MinHook resumes it. Never destroy live trampolines.
                    entry.original.store(reinterpret_cast<Function>(original), std::memory_order_release);
                }
                if (!entry.enabled) {
                    if (MH_EnableHook(target) != MH_OK) { return false; }
                    entry.enabled = true;
                    count.fetch_add(1, std::memory_order_relaxed);
                }
                return true;
            }
            return false;
        }
    }

    bool Ensure(ID3D11DeviceContext* context, DispatchObserver dispatch, CopyObserver copy)
    {
        if (!context || !dispatch || !copy || depth) { return false; }
        std::lock_guard lock(installMutex);
        static const auto initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) { return false; }
        const auto* table = *reinterpret_cast<void***>(context);
        static const auto dispatchHooks = DispatchHooks(std::make_index_sequence<Capacity>{});
        static const auto copyHooks = CopyHooks(std::make_index_sequence<Capacity>{});
        const bool dispatchOK = Install(table[41], dispatch, dispatchEntries, dispatchHooks, dispatchCount);
        const bool copyOK = Install(table[47], copy, copyEntries, copyHooks, copyCount);
        return dispatchOK && copyOK;
    }
    std::size_t DispatchEntries() { return dispatchCount.load(std::memory_order_relaxed); }
    std::size_t CopyEntries() { return copyCount.load(std::memory_order_relaxed); }
}
