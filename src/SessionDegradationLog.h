#pragma once
#include "SessionDegradation.h"
#include <spdlog/spdlog.h>

namespace TheosRenderPipeline::Degradation
{
    // Record the degradation for the menu and write its one log line.
    inline void Note(Reason reason, std::string_view detail)
    {
        if (Session().Report(reason, detail)) { spdlog::warn("{}", LogLine(reason, detail)); }
    }
    inline void Restored(Reason reason)
    {
        if (Session().Resolve(reason)) { spdlog::info("[Degraded] {}: restored ({})", Name(FeatureOf(reason)), Name(reason)); }
    }
}
