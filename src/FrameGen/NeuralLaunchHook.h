#pragma once
#include "NeuralLaunchRequirements.h"
#include <Windows.h>
#include <d3d12.h>
#include <mutex>
#include <format>
#include <cstddef>
#include <set>

namespace TheosRenderPipeline::NeuralRendering
{
    // NVAPI CU descriptor ABI (nvapi.h); parameter pointers are borrowed unchanged.
    struct KernelLaunch { void* function; LaunchDim grid, block; std::uint32_t sharedBytes; const void* params; std::uint32_t paramSize; };
    struct KernelLaunchEx { void* function; LaunchDim grid, block; std::uint32_t sharedBytes; const void* params; std::uint32_t paramSize; void** kernelParams; };

    static_assert(sizeof(KernelLaunch) == 56 && sizeof(KernelLaunchEx) == 64);
    static_assert(offsetof(KernelLaunch, block) == 20 && offsetof(KernelLaunch, params) == 40 &&
        offsetof(KernelLaunchEx, kernelParams) == 56, "NVAPI CU launch descriptor ABI");

    class LaunchBindings
    {
    public:
        LaunchRequirements requirements;
        std::map<void*, std::string> names;
        std::string failure;
        int capability{61};
        bool Ready() const {
            for (const auto& [name, rule] : requirements) {
                if (!rule.block.x) { continue; } // Bound-only entries are checked when launched.
                bool found = false;
                for (const auto& [handle, bound] : names) { found |= bound == name; }
                if (!found) { return false; }
            }
            return failure.empty();
        }
        template<class T> bool Rewrite(const T* launches, std::uint32_t count, std::vector<T>& result) {
            result.clear();
            auto fail = [&](std::string reason) { failure = reason; result.clear(); return false; };
            if (!failure.empty()) { return false; }
            if (count && !launches) { return fail("NR launch rejected: invalid chain or previous launch failure"); }
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto name = names.find(launches[i].function);
                if (name == names.end()) { return fail("NR launch rejected: unknown CU function handle"); }
                const auto rule = requirements.find(name->second);
                if (rule == requirements.end()) { continue; }
                if (result.empty()) { result.assign(launches, launches+count); }
                auto& launch = result[i];
                if (rule->second.block.x) { launch.block = rule->second.block; }
                if (rule->second.flags & 1) { launch.grid = rule->second.grid; }
                if (rule->second.flags & 4) {
                    const auto& m = rule->second.grid;
                    const std::uint64_t x = std::uint64_t(launch.grid.x)*m.x;
                    const std::uint64_t y = std::uint64_t(launch.grid.y)*m.y;
                    const std::uint64_t z = std::uint64_t(launch.grid.z)*m.z;
                    if (!x || x > INT32_MAX || !y || y > 65535 || !z || z > 65535) {
                        return fail("NR launch rejected: multiplied grid exceeds CUDA limits");
                    }
                    launch.grid = {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(z)};
                }
                if (rule->second.flags & 2) { launch.sharedBytes = rule->second.sharedBytes; }
                const auto& bound = rule->second.bounds[capability == 89 ? 1 : 0];
                if (bound.maxThreads && (std::uint64_t(launch.block.x)*launch.block.y*launch.block.z > bound.maxThreads ||
                        (bound.requiredBlock.x && launch.block != bound.requiredBlock) || launch.sharedBytes > bound.maxDynamicShared)) {
                    return fail(std::format("NR launch rejected before NVAPI: {} block={}x{}x{} shared={} exceeds compiled native bounds threads<={} req={}x{}x{} dynamicShared<={}",
                        name->second,launch.block.x,launch.block.y,launch.block.z,launch.sharedBytes,bound.maxThreads,
                        bound.requiredBlock.x,bound.requiredBlock.y,bound.requiredBlock.z,bound.maxDynamicShared));
                }
            }
            return true;
        }
    };

    // Installed before NR initialization; retained with the pinned runtime because
    // NVAPI function pointers are cached by the DLL across feature recreations.
    bool InstallLaunchHook(HMODULE runtime, const LaunchRequirements&, int capability, std::string& reason);
    bool LaunchHookReady(HMODULE runtime, std::string& reason);
    bool LaunchHookHealthy(HMODULE runtime, std::string& reason);
}
