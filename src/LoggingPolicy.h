#pragma once
#include <Windows.h>
#include <spdlog/spdlog.h>

namespace TheosRenderPipeline::Logging
{
    inline void ConfigureFlush(spdlog::logger& logger)
    {
        // Routine records are flushed periodically by the registered logger.
        // Failures must still reach disk immediately.
        logger.flush_on(spdlog::level::warn);
    }

    inline bool NativeUITraceEnabled()
    {
        // Read once before any per-frame menu or graphics-state inspection.
        // Set TRP_TRACE_NATIVE_UI=1 in the game's environment before startup.
        static const bool enabled = [] {
            wchar_t value[2]{};
            return GetEnvironmentVariableW(L"TRP_TRACE_NATIVE_UI", value, 2) == 1 && value[0] == L'1';
        }();
        return enabled;
    }
}
