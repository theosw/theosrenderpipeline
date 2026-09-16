#pragma once

#include "UpscaleType.h"
#include <filesystem>
#include <string_view>

namespace TheosRenderPipeline
{
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
    inline bool ExplicitTestSaveName(std::string_view name)
    {
        return !name.empty() && name != "." && name != ".." &&
            name.find_first_of("/\\:*?\"<>|\r\n") == std::string_view::npos;
    }
#endif
    // Missing legacy selectors use the NVIDIA defaults. Explicit requests for
    // unavailable owners remain configuration errors.
    template <class Ini> const char* ValidateNvidiaBaseline(const Ini& ini)
    {
        const auto mode = ini.GetLongValue("Settings", "UpscaleType", DLSS);
        if (!ini.GetBoolValue("Settings", "EnableUpscaler", true) || (mode != DLSS && mode != DLAA)) {
            return "This renderer requires DLSS or DLAA. Use the Frame generation checkbox to turn interpolation off.";
        }
        if (ini.GetBoolValue("Experimental", "PureDarkFullDelegation", false) ||
            ini.GetLongValue("Experimental", "FrameGenerationBackend", 1) != 1 ||
            !ini.GetBoolValue("Experimental", "SourceDLSSGBackend", true)) {
            return "This configuration selects an unavailable renderer. Use the packaged NVIDIA settings.";
        }
        if (ini.GetLongValue("Experimental", "X3PresentationMode", 0) != 0 ||
            ini.GetBoolValue("Experimental", "EnableX3Presentation", false) ||
            ini.GetLongValue("Experimental", "NeuralRenderingStartupMode", 0) != 0) {
            return "Legacy presentation experiments are unavailable; use the SourceDLSSG settings for MFG and Neural Rendering.";
        }
        if (ini.GetBoolValue("Experimental", "EnableXessCapabilityProbe", false) ||
            ini.GetBoolValue("Experimental", "EnableNeuralRenderingCapabilityProbe", false) ||
            ini.GetBoolValue("Debug", "AutoABTest", false)) {
            return "Legacy probes and automatic A/B are disabled in this baseline.";
        }
#if defined(ARP_DEVELOPER_DIAGNOSTICS)
        if (ini.GetBoolValue("Debug", "AutoLoadSave", false) &&
            !ExplicitTestSaveName(ini.GetValue("Debug", "AutoLoadSaveName", ""))) {
            return "Automatic test loading requires an explicit AutoLoadSaveName (save basename, not a path).";
        }
#endif
        return nullptr;
    }

    inline std::filesystem::path ResolveRuntimePath(
        const std::filesystem::path& configured, const std::filesystem::path& pluginDirectory)
    {
        if (configured.empty() || configured.is_absolute()) { return configured; }
        return (pluginDirectory / configured).lexically_normal();
    }
}
