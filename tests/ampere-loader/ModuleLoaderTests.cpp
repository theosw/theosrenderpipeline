#include "extern/MFGAmpere/module_loader.hpp"
#include <detours/Detours.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace Loader = trp::ampere;
namespace Paths = TheosRenderPipeline::PluginPaths;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Module {
    HMODULE handle{};
    ~Module() { if (handle) FreeLibrary(handle); }
};

std::wstring mode, fixtureName;
fs::path reportedPath;
fs::path hookedPath;
decltype(&GetModuleFileNameW) originalModulePath{};
DWORD WINAPI ReportModulePath(HMODULE module, LPWSTR output, DWORD size) {
    wchar_t actual[32768]{};
    if (!module || !originalModulePath(module, actual, 32768) || _wcsicmp(actual, hookedPath.c_str()) != 0)
        return originalModulePath(module, output, size);
    if (mode == L"query-failed") { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    if (mode == L"query-truncated") { SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
    if (reportedPath.empty()) return originalModulePath(module, output, size);
    if (reportedPath.native().size() >= size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
    std::copy(reportedPath.native().begin(), reportedPath.native().end(), output);
    output[reportedPath.native().size()] = L'\0';
    return static_cast<DWORD>(reportedPath.native().size());
}
struct ReportedPathHook {
    uintptr_t previous{};
    void Install() {
        previous = Detours::IATHook(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),
            "kernel32.dll", "GetModuleFileNameW", reinterpret_cast<uintptr_t>(ReportModulePath));
        Require(previous != 0, "module path hook installed");
        originalModulePath = reinterpret_cast<decltype(originalModulePath)>(previous);
    }
    ~ReportedPathHook() {
        if (previous) Detours::IATHook(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),
            "kernel32.dll", "GetModuleFileNameW", previous);
    }
};

int wmain(int argc, wchar_t** argv) {
    // The malformed-image case must report its error without a Windows dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    try {
        Require(argc == 6, "fixture A, fixture B, alias directory, invalid image and mode required");
        const auto fixtureA = Paths::Normalize(argv[1]);
        const auto fixtureB = Paths::Normalize(argv[2]);
        fixtureName = fixtureA.filename();
        hookedPath = fixtureA;
        mode = argv[5];
        const bool separate = mode.starts_with(L"cs-");
        auto requested = fixtureA;
        auto expectedFailure = Loader::ModuleLoadFailure::None;
        DWORD expectedError{};
        bool expectLoaded = true;
        Module existing;
        ReportedPathHook hook;
        if (mode == L"cs-peer-first" || mode == L"cs-missing" || mode == L"cs-mismatch") {
            existing.handle = LoadLibraryW(fixtureB.c_str());
            Require(existing.handle != nullptr, "independent CS-like owner loaded first");
            if (mode == L"cs-missing") {
                requested = fixtureA.parent_path() / L"missing" / fixtureA.filename();
                expectedFailure = Loader::ModuleLoadFailure::LoadFailed;
                expectedError = ERROR_MOD_NOT_FOUND;
                expectLoaded = false;
            } else if (mode == L"cs-mismatch") {
                reportedPath = fixtureB;
                expectedFailure = Loader::ModuleLoadFailure::PathMismatch;
                hook.Install();
            }
        } else if (mode == L"cs-owned" || mode == L"cs-owned-alias") {
            existing.handle = LoadLibraryW(fixtureA.c_str());
            Require(existing.handle != nullptr, "configured module already owned");
            if (mode == L"cs-owned-alias") requested = fs::path(argv[3]) / fixtureA.filename();
            expectedFailure = Loader::ModuleLoadFailure::AlreadyOwned;
            expectLoaded = false;
        } else if (mode == L"alias") {
            reportedPath = fs::path(argv[3]) / fixtureA.filename();
            Require(_wcsicmp(reportedPath.c_str(), fixtureA.c_str()) != 0, "alias has a different spelling");
            Require(fs::equivalent(reportedPath, fixtureA), "alias resolves to the fixture");
            hook.Install();
        } else if (mode == L"foreign") {
            reportedPath = fixtureB;
            expectedFailure = Loader::ModuleLoadFailure::PathMismatch;
            hook.Install();
        } else if (mode == L"missing") {
            requested = fixtureA.parent_path() / L"MissingAmpereLoaderFixture.dll";
            expectedFailure = Loader::ModuleLoadFailure::LoadFailed;
            expectedError = ERROR_MOD_NOT_FOUND;
            expectLoaded = false;
        } else if (mode == L"invalid-image") {
            requested = fs::absolute(argv[4]);
            expectedFailure = Loader::ModuleLoadFailure::LoadFailed;
            expectedError = ERROR_BAD_EXE_FORMAT;
            expectLoaded = false;
        } else if (mode == L"query-failed" || mode == L"query-truncated") {
            expectedFailure = Loader::ModuleLoadFailure::PathQueryFailed;
            expectedError = mode == L"query-failed" ? ERROR_ACCESS_DENIED : ERROR_INSUFFICIENT_BUFFER;
            hook.Install();
        } else if (mode == L"already-owned") {
            existing.handle = LoadLibraryW(fixtureB.c_str());
            Require(existing.handle != nullptr, "foreign owner loaded");
            expectedFailure = Loader::ModuleLoadFailure::AlreadyOwned;
            expectLoaded = false;
        } else if (mode == L"relative") {
            requested = fixtureA.filename();
            expectedFailure = Loader::ModuleLoadFailure::InvalidPath;
            expectedError = ERROR_BAD_PATHNAME;
            expectLoaded = false;
        } else Require(mode == L"direct" || mode == L"cs-trp-first", "known fixture mode");

        const auto result = Loader::LoadConfiguredModule(requested, separate);
        Module owned{result.module};
        Require(result.failure == expectedFailure, "specific failure classification");
        Require(result.windowsError == expectedError, "original Windows error retained");
        Require(result.Succeeded() == (expectedFailure == Loader::ModuleLoadFailure::None), "only validated module accepted");
        Require((result.module != nullptr) == expectLoaded, "actual loaded reference returned even after verification failure");
        if (result.module) {
            auto identity = reinterpret_cast<int(*)()>(GetProcAddress(result.module, "ModuleIdentity"));
            Require(identity && identity() == 1, "loaded fixture remains valid for the owner");
        }
        if (result.Succeeded()) {
            Module retained{Paths::RetainLoadedModule(requested)};
            Require(retained.handle == result.module, "post-startup retention uses the same configured identity");
            if (mode == L"cs-trp-first") {
                existing.handle = LoadLibraryW(fixtureB.c_str());
                Require(existing.handle != nullptr, "independent CS-like owner loaded second");
            }
            if (separate) {
                Require(existing.handle != result.module, "separate owners retain separate modules");
                const auto repeat = Loader::LoadConfiguredModule(requested, true);
                Module repeatOwner{repeat.module};
                Require(!repeat.module && repeat.failure == Loader::ModuleLoadFailure::AlreadyOwned,
                    "CS coexistence never admits a second owner of the configured module");
            }
        }
        if (separate && existing.handle && mode != L"cs-owned" && mode != L"cs-owned-alias") {
            auto identity = reinterpret_cast<int(*)()>(GetProcAddress(existing.handle, "ModuleIdentity"));
            Require(identity && identity() == 2, "CS-like module remains intact");
            Module retainedPeer{Paths::RetainLoadedModule(fixtureB)};
            Require(retainedPeer.handle == existing.handle, "CS-like module keeps its own path identity");
        }
        if (mode == L"alias" || mode == L"foreign") {
            Require(result.reported == reportedPath, "diagnostic keeps original reported spelling");
            Require((result.normalizedRequested == result.normalizedReported) == (mode == L"alias"), "normalization respects location");
        }
        SetLastError(ERROR_INVALID_FUNCTION);
        const auto diagnostic = Loader::ModuleLoadDiagnostic(result);
        Require(diagnostic.find(result.Error()) != std::string::npos, "diagnostic identifies failure stage");
        Require(diagnostic.find(requested.filename().string()) != std::string::npos, "diagnostic identifies module");
        Require(diagnostic.find("win32=" + std::to_string(expectedError)) != std::string::npos, "diagnostic uses captured error");
        std::cout << diagnostic << '\n' << "PASS: production Ampere loader\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
