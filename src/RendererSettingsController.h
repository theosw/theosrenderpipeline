#pragma once

#include "RendererSettings.h"
#include <string>

class RenderPipeline;
class SourceFrameGeneration;
class NvidiaHost;
class PerformanceTuning;

namespace TheosRenderPipeline
{
struct RendererSettingsResult
{
    std::string message;
    bool error{};
    bool applied{};
};

// Borrows the current owners. Requested/effective/persisted state stays with
// the host; the overlay supplies a draft and displays the result.
class RendererSettingsController
{
  public:
    static RendererSettingsController Current();
    RendererSettingsDraft Capture(bool nrRuntimePresent, bool readTextures = true) const;
    int CountChanges(const RendererSettingsDraft& draft, bool nrRuntimePresent) const;
    RendererSettingsResult Apply(const RendererSettingsDraft& draft, bool save);
    RendererSettingsResult SetNeuralRenderingEnabled(bool enabled);

  private:
    RendererSettingsController(RenderPipeline& upscaler, SourceFrameGeneration& frameGen, NvidiaHost& host,
                               PerformanceTuning& performance, TextureProviderBridge& textures)
        : upscaler_(upscaler), frameGen_(frameGen), host_(host), performance_(performance), textures_(textures)
    {
    }
    RenderPipeline& upscaler_;
    SourceFrameGeneration& frameGen_;
    NvidiaHost& host_;
    PerformanceTuning& performance_;
    TextureProviderBridge& textures_;
};
} // namespace TheosRenderPipeline
