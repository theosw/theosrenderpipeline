#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <cstddef>
#include <cstdint>

// Optional companion to the private tensor ABI; older tensor helpers remain usable.
// Context values are opaque identities. Register after successful SDK initialization;
// unregister only after successful SDK destruction has joined its presentation threads.
namespace TheosRenderPipeline::XeFGPacing
{
    inline constexpr std::uint32_t Version = 2;
    enum Flags : std::uint32_t { Installed=1, Registered=2, DriverAvailable=4, Requested=8, QueueReady=16, Metered=32, Unsafe=64, Unavailable=128, VSyncCadenceRequested=256 };
    // Version 2 adds the session-only software VSync cadence switch.
    constexpr bool CanMeter(std::uint32_t flags) {
        constexpr auto required = Installed | Registered | DriverAvailable | QueueReady;
        return (flags & required) == required && !(flags & (Unsafe | Unavailable));
    }
    struct Stats {
        std::uint32_t size{sizeof(Stats)}, flags{};
        std::uint64_t deadlines{}, extraPaces{}, flipCalls{}, meteredCalls{}, driverFailures{}, resets{};
        std::uint32_t feedback{};
        std::int32_t driverStatus{};
    };
    struct API {
        std::uint32_t size{sizeof(API)}, version{Version};
        HRESULT (__cdecl* registerContext)(void*, ID3D12Device*, char*, std::uint32_t){};
        HRESULT (__cdecl* unregisterContext)(void*, char*, std::uint32_t){};
        void (__cdecl* reset)(void*){};
        void (__cdecl* setMetering)(bool){};
        void (__cdecl* snapshot)(Stats*){};
        void (__cdecl* setLogger)(void (*)(const char*)){};
        void (__cdecl* setVSyncCadence)(bool){};
    };
    // A version-1 table is the same layout without setVSyncCadence. Either side accepts it,
    // so mixed renderer/helper files keep pacing and metering; only VSync cadence is absent.
    inline constexpr std::uint32_t Version1 = 1;
    inline constexpr std::uint32_t Version1Size = offsetof(API, setVSyncCadence);
    using GetAPI = HRESULT (__cdecl*)(API*);
}
