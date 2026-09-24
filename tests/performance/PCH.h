#pragma once

// The fixture compiles the production recorder without SKSE/game initialization.
// Only the unrelated text logging service is replaced; no query or scope code is.
namespace logger
{
    template<class... Args> void info(const char*, Args&&...) {}
    template<class... Args> void warn(const char*, Args&&...) {}
    template<class... Args> void error(const char*, Args&&...) {}
}
