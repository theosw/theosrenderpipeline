#pragma once
#include <format>
#include <string>
#include <string_view>

namespace TheosRenderPipeline::SourceDLSSG
{
inline std::string RuntimeModuleFailureMessage(std::string_view name, bool frameGenerationOverrideObserved)
{
    auto message = std::format("{} was not loaded from the configured Streamline directory", name);
    if (name == "nvngx_dlssg.dll" && frameGenerationOverrideObserved) {
        message += ". NVIDIA reported a DLSS Frame Generation override. In NVIDIA App's Skyrim profile, set "
            "DLSS Override - Model Presets (Frame Generation) and DLSS Override - Frame Generation to "
            "Use the 3D application setting, then restart Skyrim. See TheosRenderPipeline.log for paths";
    }
    return message;
}
}
