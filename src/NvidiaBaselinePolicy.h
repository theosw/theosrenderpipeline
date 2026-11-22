#pragma once

#include "UpscaleType.h"
#include <filesystem>
#include <string>
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
    // UpscaleType values this renderer runs; anything else (the retired 1 and 2,
    // a typo) starts as DLSS for the session. The saved file is not rewritten.
    inline constexpr int SupportedUpscaleType(long mode)
    {
        return mode == DLSS || mode == DLAA || mode == XeSS || IsFSR(static_cast<int>(mode)) ? static_cast<int>(mode) : DLSS;
    }

    // Old or invalid selectors in the settings files, as "Section/Key" separated
    // by commas; empty when there are none. No reader uses these keys any more,
    // so they are ignored for the session and named in the menu. Nothing is
    // migrated and the files are left as they are.
    template <class Ini> std::string IgnoredLegacySettings(const Ini& ini)
    {
        std::string ignored;
        const auto add = [&](const char* key) { ignored += (ignored.empty() ? "" : ", "); ignored += key; };
        if (!ini.GetBoolValue("Settings", "EnableUpscaler", true)) { add("Settings/EnableUpscaler"); }
        const auto mode = ini.GetLongValue("Settings", "UpscaleType", DLSS);
        if (SupportedUpscaleType(mode) != mode) { add("Settings/UpscaleType"); }
        if (ini.GetBoolValue("Experimental", "PureDarkFullDelegation", false)) { add("Experimental/PureDarkFullDelegation"); }
        if (ini.GetLongValue("Experimental", "FrameGenerationBackend", 1) != 1) { add("Experimental/FrameGenerationBackend"); }
        if (!ini.GetBoolValue("Experimental", "SourceDLSSGBackend", true)) { add("Experimental/SourceDLSSGBackend"); }
        if (ini.GetLongValue("Experimental", "X3PresentationMode", 0) != 0) { add("Experimental/X3PresentationMode"); }
        if (ini.GetBoolValue("Experimental", "EnableX3Presentation", false)) { add("Experimental/EnableX3Presentation"); }
        if (ini.GetLongValue("Experimental", "NeuralRenderingStartupMode", 0) != 0) { add("Experimental/NeuralRenderingStartupMode"); }
        if (ini.GetBoolValue("Experimental", "EnableXessCapabilityProbe", false)) { add("Experimental/EnableXessCapabilityProbe"); }
        if (ini.GetBoolValue("Experimental", "EnableNeuralRenderingCapabilityProbe", false)) { add("Experimental/EnableNeuralRenderingCapabilityProbe"); }
        if (ini.GetBoolValue("Debug", "AutoABTest", false)) { add("Debug/AutoABTest"); }
        return ignored;
    }

#if defined(ARP_DEVELOPER_DIAGNOSTICS)
    // Developer builds only: automatic test loading must name its save.
    template <class Ini> const char* ValidateDeveloperSettings(const Ini& ini)
    {
        if (ini.GetBoolValue("Debug", "AutoLoadSave", false) &&
            !ExplicitTestSaveName(ini.GetValue("Debug", "AutoLoadSaveName", ""))) {
            return "Automatic test loading requires an explicit AutoLoadSaveName (save basename, not a path).";
        }
        return nullptr;
    }
#endif

    inline std::filesystem::path ResolveRuntimePath(
        const std::filesystem::path& configured, const std::filesystem::path& pluginDirectory)
    {
        if (configured.empty() || configured.is_absolute()) { return configured; }
        return (pluginDirectory / configured).lexically_normal();
    }
}
