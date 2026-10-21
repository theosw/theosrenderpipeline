#pragma once
#include "NeuralLaunchRequirements.h"
#include <Windows.h>
#include <d3d12.h>
#include <mutex>
#include <set>

namespace TheosRenderPipeline::NeuralRendering
{
    // NVAPI CU descriptor ABI (nvapi.h); parameter pointers are borrowed unchanged.
    struct KernelLaunch { void* function; LaunchDim grid, block; std::uint32_t sharedBytes; const void* params; std::uint32_t paramSize; };
    struct KernelLaunchEx { void* function; LaunchDim grid, block; std::uint32_t sharedBytes; const void* params; std::uint32_t paramSize; void** kernelParams; };

    class LaunchBindings
    {
    public:
        LaunchRequirements requirements;
        std::map<void*, std::string> names;
        std::string failure;
        bool Ready() const {
            for (const auto& [name, rule] : requirements) {
                bool found = false;
                for (const auto& [handle, bound] : names) { found |= bound == name; }
                if (!found) { return false; }
            }
            return failure.empty();
        }
        template<class T> bool Rewrite(const T* launches, std::uint32_t count, std::vector<T>& result) {
            result.clear();
            if (!failure.empty() || (count && !launches)) { failure = "NR launch rejected: invalid chain or previous launch failure"; return false; }
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto name = names.find(launches[i].function);
                if (name == names.end()) { failure = "NR launch rejected: unknown CU function handle"; return false; }
                const auto rule = requirements.find(name->second);
                if (rule == requirements.end()) { continue; }
                if (result.empty()) { result.assign(launches, launches+count); }
                auto& launch = result[i];
                launch.block = rule->second.block;
                if (rule->second.flags & 1) { launch.grid = rule->second.grid; }
                if (rule->second.flags & 2) { launch.sharedBytes = rule->second.sharedBytes; }
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
