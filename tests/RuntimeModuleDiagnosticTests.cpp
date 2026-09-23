#include "FrameGen/SourceDLSSGRuntimeDiagnostics.h"
#include "FrameGen/SourceRuntimeModuleDiagnostic.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace TheosRenderPipeline::SourceDLSSG;
void Require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
int main()
{
    try {
        RuntimeDiagnostics diagnostics;
        diagnostics.Record("Feature dlss override enabled");
        diagnostics.Record("Feature dlssg override disabled");
        diagnostics.Record("Built with APP_NAME = nvapp_override");
        Require(!diagnostics.FrameGenerationOverrideObserved(), "other features and generic app names are not FG override proof");
        const auto ordinary = RuntimeModuleFailureMessage("nvngx_dlssg.dll", false);
        Require(ordinary.find("NVIDIA reported") == std::string::npos, "no unsupported override diagnosis without evidence");
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] { for (int n = 0; n < 100; ++n) {
                diagnostics.Record("[NGXSecureLoadFeature:1348] Feature dlssg override enabled");
                diagnostics.Record("RSYNC: Fence wait error");
            } });
        }
        for (auto& thread : threads) { thread.join(); }
        Require(diagnostics.FrameGenerationOverrideObserved(), "retain exact vendor FG-override observation across callbacks");
        Require(diagnostics.Snapshot().Count(RuntimeDiagnosticEvent::SynchronizationFailure) == 400, "existing counters remain intact");
        const auto overrideMessage = RuntimeModuleFailureMessage("nvngx_dlssg.dll", diagnostics.FrameGenerationOverrideObserved());
        Require(overrideMessage.find("NVIDIA reported") != std::string::npos &&
            overrideMessage.find("Use the 3D application setting") != std::string::npos, "provide specific app-profile remedy");
        Require(RuntimeModuleFailureMessage("sl.reflex.dll", true).find("NVIDIA reported") == std::string::npos,
            "FG override observation cannot mislabel another missing module");
        std::cout << "PASS: evidence-based FG override diagnosis and concurrent vendor-log capture\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
