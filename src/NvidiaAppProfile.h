#pragma once

#include <cstdint>
#include <optional>
#include <string>

// What NVIDIA App (or the driver) has set for this game in the driver profile.
// TRP never refuses to run because of these; the menu marks what they change.
namespace TheosRenderPipeline::NvidiaApp
{
    // Driver profile (DRS) setting IDs, as the driver names them. NVIDIA App marks
    // these DLSS overrides Unsupported for Skyrim, so they come from tools such as
    // NVIDIA Profile Inspector. Smooth Motion is not here: NVIDIA App stores it in
    // a setting the public DRS API refuses to return (0x0005F543), so TRP detects
    // it at run time instead (SmoothMotionLoaded).
    inline constexpr std::uint32_t kUpscalingEnable = 0x10E41E01;      // Enable DLSS-SR override
    inline constexpr std::uint32_t kUpscalingPreset = 0x10E41DF3;      // Override DLSS-SR presets
    inline constexpr std::uint32_t kUpscalingForceDLAA = 0x10E41DF4;   // Override DLSS mode to be DLAA
    inline constexpr std::uint32_t kGenerationEnable = 0x10E41E03;     // Enable DLSS-FG override
    inline constexpr std::uint32_t kGenerationPreset = 0x10E41DF1;     // Override DLSS-FG preset
    inline constexpr std::uint32_t kGenerationMode = 0x10308298;       // Override DLSSG mode
    inline constexpr std::uint32_t kGenerationFrames = 0x104D6667;     // Override DLSSG multi-frame count
    inline constexpr std::uint32_t kNeuralEnable = 0x10E41E04;         // Enable DLSS NR override
    inline constexpr std::uint32_t kNeuralPreset = 0x10E41DF8;         // Override DLSS NR presets
    inline constexpr std::uint32_t kStreamlineEnable = 0x10E41E06;     // Enable Streamline override

    // Each value from this game's profile, else the global one; empty when
    // neither sets it, which means the driver default (no override).
    struct Values
    {
        bool read{};
        std::optional<std::uint32_t> upscalingEnable, upscalingPreset, upscalingForceDLAA;
        std::optional<std::uint32_t> generationEnable, generationPreset, generationMode, generationFrames;
        std::optional<std::uint32_t> neuralEnable, neuralPreset, streamlineEnable;
    };

    struct Overrides
    {
        bool read{};
        bool upscaling{};
        std::string upscalingModel;   // "Latest", "K", "DLAA", ... when upscaling
        bool frameGeneration{};
        bool neural{};
        std::string neuralModel;
    };

    // NVIDIA's preset values: 1 is A, 0xFFFFFF is the latest model.
    inline std::string PresetName(std::uint32_t value)
    {
        if (value == 0xFFFFFF) { return "Latest"; }
        if (value >= 1 && value <= 26) { return std::string(1, static_cast<char>('A' + value - 1)); }
        return "NVIDIA App";
    }

    inline Overrides Interpret(const Values& values)
    {
        Overrides result;
        result.read = values.read;
        if (!values.read) { return result; }
        const auto on = [](const std::optional<std::uint32_t>& value) { return value.value_or(0) != 0; };
        result.upscaling = on(values.upscalingEnable);
        if (result.upscaling) {
            result.upscalingModel = on(values.upscalingForceDLAA) ? std::string("DLAA")
                                    : on(values.upscalingPreset)  ? PresetName(*values.upscalingPreset)
                                                                  : std::string("NVIDIA App");
        }
        result.frameGeneration = on(values.generationEnable) || on(values.generationMode) ||
                                 on(values.generationFrames) || on(values.streamlineEnable);
        result.neural = on(values.neuralEnable);
        if (result.neural) {
            result.neuralModel = on(values.neuralPreset) ? PresetName(*values.neuralPreset) : std::string("NVIDIA App");
        }
        return result;
    }

    // Reads the driver profile of `exeName`, by default this process's. It loads
    // the whole profile store, so call it off the render thread. Read-only.
    Values ReadProfile(const std::wstring& exeName = {});

    // Smooth Motion runs inside the game: the driver loads NvPresent64.dll when it
    // creates the swapchain for a program with Smooth Motion on (checked on an
    // RTX 4080 SUPER, driver 621.01, flip and blt D3D11 swapchains).
    bool SmoothMotionLoaded();
}
