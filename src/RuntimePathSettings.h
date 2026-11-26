#pragma once

#include "NvidiaBaselinePolicy.h"
#include "FrameGen/NeuralRuntimeFiles.h"
#include <filesystem>
#include <string>

namespace TheosRenderPipeline
{
    struct RuntimePathSettings
    {
        std::string streamline;
        std::string neural;

        template<class Ini> void Load(const Ini& ini)
        {
            streamline = ini.GetValue("Experimental", "SourceDLSSGStreamlineDirectory", "");
            neural = SourceDLSSG::NormalizeNeuralRuntimeSetting(ini.GetValue("Experimental", "NeuralRenderingRuntimePath", ""),
                ini.GetBoolValue("Experimental", "NeuralRenderingRuntimeExplicit", false));
        }

        [[nodiscard]] RuntimePathSettings Resolve(const std::filesystem::path& pluginDirectory) const
        {
            const auto toUtf8 = [](const std::filesystem::path& path) {
                const auto u8 = path.u8string();
                return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
            };
            return {toUtf8(ResolveRuntimePath(streamline, pluginDirectory)),
                toUtf8(SourceDLSSG::ResolveNeuralRuntimeSetting(neural, pluginDirectory))};
        }

        template<class Ini> void Store(Ini& ini) const
        {
            // Seed missing values only. The NR runtime is a menu choice that
            // StorePreferences writes afterwards; Streamline stays startup-owned.
            if (!ini.GetValue("Experimental", "SourceDLSSGStreamlineDirectory", nullptr)) {
                ini.SetValue("Experimental", "SourceDLSSGStreamlineDirectory", streamline.c_str());
            }
            if (!ini.GetValue("Experimental", "NeuralRenderingRuntimePath", nullptr)) {
                ini.SetValue("Experimental", "NeuralRenderingRuntimePath", neural.c_str());
            }
        }
    };
}
