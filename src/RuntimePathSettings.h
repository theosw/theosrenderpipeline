#pragma once

#include "NvidiaBaselinePolicy.h"
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
            neural = ini.GetValue("Experimental", "NeuralRenderingRuntimePath", "");
        }

        [[nodiscard]] RuntimePathSettings Resolve(const std::filesystem::path& pluginDirectory) const
        {
            return {ResolveRuntimePath(streamline, pluginDirectory).string(),
                ResolveRuntimePath(neural, pluginDirectory).string()};
        }

        template<class Ini> void Store(Ini& ini) const
        {
            // No menu control edits these startup paths. Retain an on-disk
            // edit made during this session; only seed missing values.
            if (!ini.GetValue("Experimental", "SourceDLSSGStreamlineDirectory", nullptr)) {
                ini.SetValue("Experimental", "SourceDLSSGStreamlineDirectory", streamline.c_str());
            }
            if (!ini.GetValue("Experimental", "NeuralRenderingRuntimePath", nullptr)) {
                ini.SetValue("Experimental", "NeuralRenderingRuntimePath", neural.c_str());
            }
        }
    };
}
