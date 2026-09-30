#pragma once

#include <cstddef>

namespace TheosRenderPipeline::D3D11ContextSlots
{
    // Immediate-context vtable slots, counted from IUnknown. The
    // ID3D11DeviceContext1 slots follow the base interface's 115 entries.
    // tests/D3D11ContextSlotsTests.cpp checks them against the C vtable layout.
    inline constexpr std::size_t kPSSetShaderResources = 8;
    inline constexpr std::size_t kDrawIndexed = 12;
    inline constexpr std::size_t kDraw = 13;
    inline constexpr std::size_t kOMSetRenderTargets = 33;
    inline constexpr std::size_t kOMSetBlendState = 35;
    inline constexpr std::size_t kRSSetViewports = 44;
    inline constexpr std::size_t kRSSetScissorRects = 45;
    inline constexpr std::size_t kCopySubresourceRegion = 46;
    inline constexpr std::size_t kClearView = 132;
}
