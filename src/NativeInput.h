#pragma once

#include <atomic>
#include <cstdint>

namespace TheosRenderPipeline::NativeInput
{
    // One aligned publication for both dimensions. The x64 call-site thunks
    // read this same word so a resize cannot mix an old width with a new height.
    alignas(8) inline std::uint64_t extentWord{};
    inline std::atomic_bool hooksInstalled{};

    inline void Publish(std::uint32_t width, std::uint32_t height)
    {
        const auto value = width && height && width <= 16384 && height <= 16384 ?
            (static_cast<std::uint64_t>(height) << 32) | width : 0;
        std::atomic_ref(extentWord).store(value, std::memory_order_release);
    }

    inline bool Active()
    {
        return hooksInstalled.load(std::memory_order_acquire) &&
            std::atomic_ref(extentWord).load(std::memory_order_acquire) != 0;
    }
}
