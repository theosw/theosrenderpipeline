#pragma once
#include <windows.h>
#include <cstdio>
#ifndef TRP_AMD_DIAGNOSTIC_LABEL
#define TRP_AMD_DIAGNOSTIC_LABEL "TRP-0.7.0"
#endif

namespace TRPAmdDiagnostics {
// Process creation time distinguishes PID reuse. Identical in host/runtime/gate.
inline const char* Session() {
    static const auto id = [] {
        struct Value { char text[64]{}; } result;
        FILETIME creation{}, exit{}, kernel{}, user{};
        const bool known = GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != 0;
        const auto time = (static_cast<unsigned long long>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
        std::snprintf(result.text, sizeof(result.text), "%lu-%016llx%s", GetCurrentProcessId(), time, known ? "" : "-unknown");
        return result;
    }();
    return id.text;
}
inline void Prefix(char* text, size_t size) {
    SYSTEMTIME utc{}; GetSystemTime(&utc);
    std::snprintf(text, size, "[%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %s session=%s tid=%lu] ",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
        TRP_AMD_DIAGNOSTIC_LABEL, Session(), GetCurrentThreadId());
}
}
