#pragma once

#include "../NvidiaBaselinePolicy.h"
#include "NeuralResearch.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace TheosRenderPipeline::SourceDLSSG
{
    inline constexpr std::string_view kNeuralRuntimeFolder = "TheosRenderPipeline/NVIDIA";
    // Runtimes relative to Data/SKSE/Plugins. Standard retains legacy 310.8 and
    // original 310.9.16. Universal adds a pinned RTX 20/30/40 kernel pack plus
    // the legacy INT8 alternative for RTX 20/30. See tools/nr-runtime.
    inline constexpr std::string_view kDefaultNeuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll";
    inline constexpr std::string_view kLatestNeuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_310.9.16.dll";
    inline constexpr std::string_view kInt8NeuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_int8.dll";
    // Optional standalone files from earlier packages remain selectable.
    inline constexpr std::string_view kRTX40NeuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_310.9.16_rtx40.dll";
    inline constexpr std::string_view kCompatNeuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_310.9.16_compat.dll";
    // The saved value that lets TRP choose a bundled runtime for the GPU.
    inline constexpr std::string_view kAutomaticNeuralRuntime = "Automatic";

    // Case- and separator-insensitive, as Windows paths are.
    inline std::string FoldRuntimePath(std::string_view path)
    {
        std::string text(path);
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return c == '\\' ? '/' : static_cast<char>(std::tolower(c)); });
        return text;
    }

    inline bool IsAutomaticNeuralRuntime(std::string_view value)
    {
        return FoldRuntimePath(value) == FoldRuntimePath(kAutomaticNeuralRuntime);
    }

    // Best first; missing or safely refused runtimes are skipped. Universal
    // installs compatibility kernels into original 310.9.16 on RTX 20/30/40.
    // INT8 is an older RTX 20/30 alternative, never an RTX 40 fallback. Both
    // editions retain legacy 310.8 as the final fallback and unknown-GPU route.
    inline std::vector<std::string_view> AutomaticNeuralRuntimes(int computeCapability, bool runtimeKernels = false)
    {
        // Before device creation this is only file admission, never execution.
        if (computeCapability < 0) { return { kDefaultNeuralRuntime, kLatestNeuralRuntime, kInt8NeuralRuntime }; }
        if (NeuralRendering::PascalResearchFor(computeCapability)) { return { kLatestNeuralRuntime }; }
        if (computeCapability >= 120) { return { kLatestNeuralRuntime, kDefaultNeuralRuntime }; }
        if (runtimeKernels) {
            if (computeCapability == 89) { return { kLatestNeuralRuntime, kDefaultNeuralRuntime }; }
            if (computeCapability == 75 || computeCapability == 86) {
                return { kLatestNeuralRuntime, kInt8NeuralRuntime, kDefaultNeuralRuntime };
            }
        }
        return { kDefaultNeuralRuntime };
    }

    // Before Automatic, saved INIs held the bundled 310.8 path as the only
    // choice. That path means Automatic unless it was picked explicitly since.
    inline std::string NormalizeNeuralRuntimeSetting(std::string value, bool explicitChoice)
    {
        if (value.empty() || (!explicitChoice && FoldRuntimePath(value) == FoldRuntimePath(kDefaultNeuralRuntime))) {
            return std::string(kAutomaticNeuralRuntime);
        }
        return value;
    }

    inline bool NeuralRuntimePresent(const std::filesystem::path& path)
    {
        std::error_code error;
        return !path.empty() && std::filesystem::is_regular_file(path, error) && !error;
    }

    // Before the device exists, accept any bundled candidate as a placeholder.
    // The backend selects again for the actual GPU before creating a feature.
    inline std::filesystem::path ResolveNeuralRuntimeSetting(std::string_view value,
        const std::filesystem::path& pluginDirectory, int computeCapability = -1, bool runtimeKernels = false)
    {
        if (!IsAutomaticNeuralRuntime(value)) {
            return ResolveRuntimePath(std::filesystem::u8path(value), pluginDirectory);
        }
        for (const auto candidate : AutomaticNeuralRuntimes(computeCapability, runtimeKernels)) {
            const auto path = ResolveRuntimePath(std::filesystem::u8path(candidate), pluginDirectory);
            if (NeuralRuntimePresent(path)) { return path; }
        }
        return ResolveRuntimePath(std::filesystem::u8path(kDefaultNeuralRuntime), pluginDirectory);
    }

    // Friendly names for bundled and commonly installed runtime filenames.
    // Unknown filenames stay visible; selection and saved paths are unchanged.
    inline std::string NeuralRuntimeLabel(std::string_view value)
    {
        if (IsAutomaticNeuralRuntime(value)) { return "Automatic"; }
        auto filename = [](std::string_view path) {
            const auto utf8 = std::filesystem::u8path(path).filename().u8string();
            return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        };
        const auto name = filename(value);
        const auto folded = FoldRuntimePath(name);
        if (folded == FoldRuntimePath(filename(kDefaultNeuralRuntime))) { return "310.8 - FP8/FP16 (legacy)"; }
        if (folded == FoldRuntimePath(filename(kLatestNeuralRuntime))) { return "310.9.16 - FP8/FP16 (NVIDIA)"; }
        if (folded == FoldRuntimePath(filename(kRTX40NeuralRuntime))) { return "310.9.16 - FP8 (TRP, RTX 40)"; }
        if (folded == FoldRuntimePath(filename(kCompatNeuralRuntime))) { return "310.9.16 - FP8/FP16 (Community)"; }
        if (folded == "nvngx_dlssnr_int8.dll") { return "310.8 - INT8 (Community v2)"; }
        if (folded == "nvngx_dlssnr_int8_ada.dll") { return "310.8 - INT8 (Community v2, RTX 40 fix)"; }
        if (folded == "nvngx_dlssnr_pd_build21.dll") { return "310.9.16 - FP8/FP16 (PureDark Build 21)"; }
        return name;
    }

    // Automatic, then every nvngx_dlssnr*.dll in the NVIDIA folder as INI paths
    // relative to Data/SKSE/Plugins, NVIDIA's default name first. `current` is
    // appended when it names a file elsewhere, so a custom path stays selectable.
    inline std::vector<std::string> ListNeuralRuntimes(const std::filesystem::path& pluginDirectory, const std::string& current)
    {
        std::vector<std::string> names;
        std::error_code error;
        for (std::filesystem::directory_iterator it(pluginDirectory / std::filesystem::u8path(kNeuralRuntimeFolder), error), end;
             !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error)) { continue; }
            const auto utf8 = it->path().filename().u8string();
            const std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            const auto folded = FoldRuntimePath(name);
            if (folded.starts_with("nvngx_dlssnr") && folded.ends_with(".dll")) { names.push_back(name); }
        }
        std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
            const bool aDefault = FoldRuntimePath(a) == "nvngx_dlssnr.dll", bDefault = FoldRuntimePath(b) == "nvngx_dlssnr.dll";
            return aDefault != bDefault ? aDefault : FoldRuntimePath(a) < FoldRuntimePath(b);
        });
        std::vector<std::string> runtimes{ std::string(kAutomaticNeuralRuntime) };
        for (const auto& name : names) { runtimes.push_back(std::string(kNeuralRuntimeFolder) + "/" + name); }
        if (!current.empty() && std::none_of(runtimes.begin(), runtimes.end(),
                [&](const std::string& r) { return FoldRuntimePath(r) == FoldRuntimePath(current); })) {
            runtimes.push_back(current);
        }
        return runtimes;
    }
}
