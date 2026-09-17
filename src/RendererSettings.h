#pragma once

#include "TextureProviderBridge.h"
#include "UpscaleType.h"
#include "FrameGen/SourceDLSSGSettings.h"
#include <array>
#include <cmath>

namespace TheosRenderPipeline
{
struct RendererSettingsDraft
{
    bool valid{false};
    int upscaleType{DLSS};
    int qualityLevel{2};
    int dlssPreset{11};
    bool autoExposure{true};
    bool sharpening{true};
    float sharpness{0.672f};
    bool enableJitter{true};
    bool nativeUI{true};
    bool requestLoadingArtwork{true};
    bool lateOverlayBridge{true};
    bool enableGPUTimings{true};
    bool enableFrameTrace{false};
    bool directRCASOutput{false};
    bool directDLSSOutput{false};
    TheosRenderPipeline::SourceDLSSG::Preferences sourceDLSSG;
    bool textureProviderConnected{false};
    TextureProviderBridge::Settings textureProviderSettings{};
};

inline int CountRendererSettingsChanges(const RendererSettingsDraft& draft, const RendererSettingsDraft& current)
{
    if (!draft.valid)
    {
        return 0;
    }
    constexpr std::array flags{&RendererSettingsDraft::autoExposure,     &RendererSettingsDraft::sharpening,
                               &RendererSettingsDraft::enableJitter,    &RendererSettingsDraft::nativeUI,
                               &RendererSettingsDraft::lateOverlayBridge, &RendererSettingsDraft::enableGPUTimings,
                               &RendererSettingsDraft::enableFrameTrace, &RendererSettingsDraft::directRCASOutput,
                               &RendererSettingsDraft::directDLSSOutput, &RendererSettingsDraft::requestLoadingArtwork};
    constexpr std::array choices{&RendererSettingsDraft::upscaleType, &RendererSettingsDraft::qualityLevel,
                                 &RendererSettingsDraft::dlssPreset};
    int count = 0;
    for (auto field : flags)
    {
        count += draft.*field != current.*field;
    }
    for (auto field : choices)
    {
        count += draft.*field != current.*field;
    }
    count += std::abs(draft.sharpness - current.sharpness) > 0.0001f;
    count += draft.sourceDLSSG != current.sourceDLSSG;
    if (draft.textureProviderConnected && current.textureProviderConnected)
    {
        count += draft.textureProviderSettings.enabled != current.textureProviderSettings.enabled;
        for (std::size_t i = 0; i < current.textureProviderSettings.maxSize.size(); ++i)
        {
            count += draft.textureProviderSettings.maxSize[i] != current.textureProviderSettings.maxSize[i];
        }
    }
    return count;
}

struct RendererSettingsCapabilities
{
    bool sourceHost{}, neuralRuntime{}, dedicatedUI{};
};

inline const char* ValidateRendererSettings(const RendererSettingsDraft& draft,
                                            RendererSettingsCapabilities capabilities)
{
    if (!capabilities.sourceHost)
    {
        return "NVIDIA host is unavailable; settings were not applied.";
    }
    if (draft.upscaleType != DLSS && draft.upscaleType != DLAA)
    {
        return "Choose DLSS or DLAA.";
    }
    if (!TheosRenderPipeline::SourceDLSSG::ValidGenerationRequest(draft.sourceDLSSG.generation))
    {
        return "Dynamic target output FPS must be 0 or between 61 and 1000.";
    }
#if !defined(TRP_BASE_RENDERER)
    if (draft.sourceDLSSG.neuralEnabled && !capabilities.neuralRuntime)
    {
        return "NR runtime DLL not found. Install nvngx_dlssnr.dll at the configured path and restart Skyrim.";
    }
    if (draft.sourceDLSSG.neuralEnabled && (draft.upscaleType != DLSS || !capabilities.dedicatedUI))
    {
        return "Source NR requires DLSS, dedicated UI Texture mode, and a configured NR runtime.";
    }
#endif
    return nullptr;
}
} // namespace TheosRenderPipeline
