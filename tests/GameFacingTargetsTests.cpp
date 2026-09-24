#include "FrameGen/GameFacingTargets.h"

#include <cstdio>
#include <cstdlib>

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

int main()
{
    using Targets = TheosRenderPipeline::GameFacingTargets;

    // A representative presentation descriptor: the inner swapchain contract
    // the host reshapes into its handoff targets.
    D3D11_TEXTURE2D_DESC output{};
    output.Width = 5120;
    output.Height = 1440;
    output.MipLevels = 1;
    output.ArraySize = 1;
    output.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    output.SampleDesc.Count = 1;
    output.Usage = D3D11_USAGE_DEFAULT;
    output.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    output.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    constexpr UINT renderWidth = 3413;
    constexpr UINT renderHeight = 960;

    const auto gameFacing = Targets::GameFacingDesc(output, renderWidth, renderHeight);
    Require(gameFacing.Width == renderWidth && gameFacing.Height == renderHeight,
        "the game-facing texture carries the render extent");
    Require((gameFacing.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0,
        "the game-facing texture retains its unordered-access binding");
    Require(gameFacing.MiscFlags == output.MiscFlags,
        "the game-facing texture preserves the inner shared-resource contract");

    const auto input = Targets::UpscaleInputDesc(output, renderWidth, renderHeight);
    Require(input.Width == renderWidth && input.Height == renderHeight,
        "the upscale input is allocated at the render extent");
    Require(input.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET),
        "the upscale input stays a plain SRV/RTV copy destination");
    Require(input.MiscFlags == 0 && input.CPUAccessFlags == 0 && input.Usage == D3D11_USAGE_DEFAULT,
        "the upscale input drops the inner metadata it cannot honor");

    // The proven path: DLSS evaluates into its own UAV and copies here.
    const auto handoff = Targets::UpscaleOutputDesc(output);
    Require(handoff.Width == output.Width && handoff.Height == output.Height,
        "the handoff target is allocated at the output extent");
    Require(handoff.Format == output.Format,
        "the handoff target matches the presentation format for CopyResource");
    Require(handoff.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET),
        "the default handoff target keeps exactly the proven bindings");

    // The opt-in direct-output routes bind this texture as the NGX or RCAS
    // destination. Without the unordered-access binding every route request is
    // rejected for the session and the per-frame copy can never be removed.
    const auto direct = Targets::UpscaleOutputDesc(output, true);
    Require((direct.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0,
        "the direct-output handoff target is unordered-access capable");
    Require(direct.BindFlags == (handoff.BindFlags | D3D11_BIND_UNORDERED_ACCESS),
        "requesting unordered access only adds that binding");

    // DLSSBackend::EnsureDirectDestinationUAV accepts a destination only when
    // every one of these holds alongside the binding.
    Require(direct.MipLevels == 1 && direct.ArraySize == 1 && direct.SampleDesc.Count == 1 &&
            direct.Usage == D3D11_USAGE_DEFAULT,
        "the direct-output handoff target satisfies the NGX destination contract");
    Require(direct.Width == output.Width && direct.Height == output.Height &&
            direct.Format == handoff.Format,
        "requesting unordered access changes no other allocation input");

    std::printf("GameFacingTargets contracts hold\n");
    return 0;
}
