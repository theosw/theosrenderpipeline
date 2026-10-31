#pragma once
#include <cstdint>
namespace TheosRenderPipeline::NeuralRendering::FaultInjection
{
    enum class Point { None, Metadata, Install, Bind, Chain, Evaluate, Creation, Allocation, Release, Shutdown, Retirement, NVAPI };
#if defined(TRP_NR_VENDOR_BENCHMARK) || defined(TRP_NR_RECOVERY_TEST)
    // Standalone fixtures only. No env/INI/export surface in a shipped plugin.
    inline Point point{};
    inline unsigned remaining{}, hits{};
    inline void Arm(Point p, unsigned occurrence = 1) { point = p; remaining = occurrence; hits = 0; }
    inline bool Hit(Point p)
    {
        if (point != p || !remaining || --remaining) { return false; }
        ++hits; point = Point::None; return true;
    }
#else
    inline bool Hit(Point) { return false; }
#endif
}
