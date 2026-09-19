#pragma once
#include "../../src/PluginPaths.h"
#include <iomanip>
#include <sstream>

namespace trp::ampere {
enum class ModuleLoadFailure { None, InvalidPath, AlreadyOwned, LoadFailed, PathQueryFailed, PathMismatch };

struct ModuleLoadResult {
    // The caller owns any returned LoadLibrary reference, including after failed
    // path verification. Startup retains it until process exit; do not unload a
    // partially prepared runtime while another startup component may reference it.
    HMODULE module{};
    ModuleLoadFailure failure{ModuleLoadFailure::None};
    DWORD windowsError{};
    std::filesystem::path requested, reported, normalizedRequested, normalizedReported;
    bool Succeeded() const { return module && failure == ModuleLoadFailure::None; }
    const char* Error() const {
        switch (failure) {
        case ModuleLoadFailure::None: return "Ampere runtime loaded from its configured path";
        case ModuleLoadFailure::InvalidPath: return "Ampere runtime requires an absolute configured file path";
        case ModuleLoadFailure::AlreadyOwned: return "Ampere runtime was already loaded by another owner";
        case ModuleLoadFailure::LoadFailed: return "Ampere runtime DLL load failed";
        case ModuleLoadFailure::PathQueryFailed: return "Ampere loaded-module path query failed";
        case ModuleLoadFailure::PathMismatch: return "Ampere runtime did not load from its configured path";
        }
        return "Ampere runtime load failed";
    }
};

inline ModuleLoadResult LoadConfiguredModule(const std::filesystem::path& requested) {
    ModuleLoadResult result;
    result.requested = requested.lexically_normal();
    if (!result.requested.is_absolute() || result.requested.filename().empty()) {
        result.failure = ModuleLoadFailure::InvalidPath;
        result.windowsError = ERROR_BAD_PATHNAME;
        return result;
    }
    if (::GetModuleHandleW(result.requested.filename().c_str())) {
        result.failure = ModuleLoadFailure::AlreadyOwned;
        return result;
    }
    result.module = ::LoadLibraryExW(result.requested.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!result.module) {
        result.windowsError = ::GetLastError();
        result.failure = ModuleLoadFailure::LoadFailed;
        return result;
    }
    wchar_t reported[32768]{};
    const auto count = ::GetModuleFileNameW(result.module, reported, 32768);
    if (!count || count >= 32768) {
        result.windowsError = ::GetLastError();
        result.failure = ModuleLoadFailure::PathQueryFailed;
        return result;
    }
    result.reported = reported;
    result.normalizedRequested = TheosRenderPipeline::PluginPaths::Normalize(result.requested);
    result.normalizedReported = TheosRenderPipeline::PluginPaths::Normalize(result.reported);
    // MO2 may report a virtual filename for a module loaded by physical path.
    // Use the same configured identity policy as post-slInit module retention.
    if (!TheosRenderPipeline::PluginPaths::EqualPath(result.reported, result.requested)) {
        result.failure = ModuleLoadFailure::PathMismatch;
    }
    return result;
}

inline std::string ModuleLoadDiagnostic(const ModuleLoadResult& result) {
    const auto utf8 = [](const std::filesystem::path& path) {
        const auto value = path.u8string();
        return std::string(value.begin(), value.end());
    };
    std::ostringstream message;
    message << result.Error() << "; module=" << std::quoted(utf8(result.requested.filename()))
        << " requested=" << std::quoted(utf8(result.requested))
        << " reported=" << std::quoted(utf8(result.reported))
        << " normalizedRequested=" << std::quoted(utf8(result.normalizedRequested))
        << " normalizedReported=" << std::quoted(utf8(result.normalizedReported))
        << " win32=" << result.windowsError;
    return message.str();
}
} // namespace trp::ampere
