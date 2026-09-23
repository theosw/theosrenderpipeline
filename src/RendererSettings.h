#pragma once

#include "TextureProviderBridge.h"
#include "UpscaleType.h"
#include "NeuralRenderingMode.h"
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
    bool reShadeBeforeUpscaling{false};
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
                               &RendererSettingsDraft::directDLSSOutput, &RendererSettingsDraft::requestLoadingArtwork,
                               &RendererSettingsDraft::reShadeBeforeUpscaling};
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
    bool sourceHost{}, neuralRuntime{}, dedicatedUI{}, externalWorld{};
    bool neuralOperational{true};
};

inline bool CanEditNeuralEnabled(bool enabled, bool available) { return enabled || available; }

inline bool SameNeuralPreferences(const SourceDLSSG::Preferences& a, const SourceDLSSG::Preferences& b)
{
    return a.neuralEnabled == b.neuralEnabled && a.neuralBeforeUpscaling == b.neuralBeforeUpscaling &&
        a.neuralPasses == b.neuralPasses && a.neuralTuning == b.neuralTuning &&
        a.neuralReconstruction == b.neuralReconstruction && a.neuralSecondPass == b.neuralSecondPass;
}

inline const char* NeuralSettingsUnavailable(int mode, RendererSettingsCapabilities capabilities)
{
    if (!capabilities.neuralRuntime) {
        return "NR runtime DLL not found. Install nvngx_dlssnr.dll at the configured path and restart Skyrim.";
    }
    if (!capabilities.neuralOperational) { return "NR is unavailable after a runtime failure; turn NR off or restart Skyrim."; }
    if (!SupportsNeuralRenderingMode(mode, capabilities.externalWorld) ||
        (!capabilities.externalWorld && !capabilities.dedicatedUI)) {
        return "NR requires dedicated UI composition. Turn NR off to apply other changes, or set NativeUICompositionMode=0 in the INI and restart Skyrim.";
    }
    return nullptr;
}

inline const char* ValidateRendererSettings(const RendererSettingsDraft& draft,
                                            RendererSettingsCapabilities capabilities,
                                            const RendererSettingsDraft* current = nullptr)
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
#if !defined(TRP_NO_NEURAL_RENDERING)
    if (draft.sourceDLSSG.neuralEnabled)
    {
        if (const auto error = NeuralSettingsUnavailable(draft.upscaleType, capabilities)) {
            // Preserve an unchanged startup request when saving unrelated edits.
            // Apply separately gates execution, so this cannot restart failed NR.
            if (!current || !SameNeuralPreferences(draft.sourceDLSSG, current->sourceDLSSG)) { return error; }
        }
    }
#endif
    return nullptr;
}
} // namespace TheosRenderPipeline
