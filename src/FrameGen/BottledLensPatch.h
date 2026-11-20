#pragma once

#include <d3d11.h>
#include <Windows.h>

namespace TheosRenderPipeline::BottledLens
{
    // Given the scene target the producer is about to draw its lens on, return
    // a layer for it to draw on with coverage in alpha, or null to leave the
    // lens on the scene. Called on the render thread from inside the producer.
    using Provider = ID3D11RenderTargetView* (*)(ID3D11RenderTargetView* scene);

    // Uncalled: not exactly one direct call to the function, or a jump to it.
    enum class State { NotInstalled, Installed, NoModule, NotFound, Ambiguous, Uncalled, HookFailed };

    // The module's linker timestamp and image size: enough to say in the log
    // which build was patched and whether it is one this code was checked on.
    struct Build
    {
        DWORD timestamp{}, imageSize{};
        bool checked{}; // Bottled Shaders V2 2.0.1.
    };

    // Redirect CameraFX::GetLensTarget in the loaded Community Shaders module,
    // in memory only. The module on disk is never written. Nothing is changed
    // unless the function is found exactly once with the known bytes and has
    // the one direct call the checked build has; the producer's own frame
    // generation keeps its answer. Once installed the redirect stays for the
    // process lifetime; the provider decides per call.
    State Install(HMODULE module, Provider provider);
    State Installed();
    Build InstalledBuild();
    const char* Describe(State state);
}
