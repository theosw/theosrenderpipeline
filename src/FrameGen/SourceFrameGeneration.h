#pragma once
#include "SourceDLSSGSettings.h"
#include <atomic>
#include <dxgi.h>
#include <string>

// Source startup policy and user requests, independent of any presenter SDK.
class SourceFrameGeneration
{
  public:
    static SourceFrameGeneration* GetSingleton()
    {
        static SourceFrameGeneration value;
        return &value;
    }
    struct Settings
    {
        bool enabled{true}; // Initial interpolation request; the NVIDIA host is always required.
        bool sourceDLSSGMFGUnlock{true}; // Matches the packaged default; explicit false is preserved.
        bool sourceDLSSGMFGUnlockPresent{};
        std::string sourceDLSSGStreamlineDirectory;
        TheosRenderPipeline::SourceDLSSG::Preferences sourceDLSSG;
        int nativeUICompositionMode{}; // 0 = dedicated UI; 1 = HUD-less detection.
        std::string neuralRenderingRuntimePath;
    };
    Settings settings;

    void LoadINI();
    // Called once before installing device hooks. Live changes do not reload
    // this startup snapshot or rebuild the presentation host.
    template<class Ini> void LoadStartupPreferences(const Ini& ini)
    {
        settings.enabled = ini.GetBoolValue("FrameGeneration", "Enabled", true);
        RequestRuntimeInterpolation(settings.enabled);
        settings.sourceDLSSGMFGUnlockPresent = ini.GetValue("Experimental", "SourceDLSSGMFGUnlock", nullptr) != nullptr;
        settings.sourceDLSSGMFGUnlock = ini.GetBoolValue("Experimental", "SourceDLSSGMFGUnlock", true);
        settings.sourceDLSSGStreamlineDirectory = ini.GetValue("Experimental", "SourceDLSSGStreamlineDirectory", "");
        settings.sourceDLSSG = TheosRenderPipeline::SourceDLSSG::LoadPreferences(ini);
        settings.nativeUICompositionMode = std::clamp(static_cast<int>(ini.GetLongValue(
            "Experimental", "NativeUICompositionMode", ini.GetLongValue("Experimental", "PureDarkHUDFixMethod", 0))), 0, 1);
        settings.neuralRenderingRuntimePath = ini.GetValue("Experimental", "NeuralRenderingRuntimePath", "");
    }
    template<class Ini> void StoreCompatibilityPreference(Ini& ini) const
    {
        // Seed older INIs when saving; never overwrite an explicit opt-out or
        // a startup preference edited on disk since this session began.
        if (!ini.GetValue("Experimental", "SourceDLSSGMFGUnlock", nullptr)) {
            ini.SetBoolValue("Experimental", "SourceDLSSGMFGUnlock", settings.sourceDLSSGMFGUnlock);
        }
    }
    template<class Ini> void StoreUIComposition(Ini& ini) const
    {
        ini.SetLongValue("Experimental", "NativeUICompositionMode", settings.nativeUICompositionMode);
        ini.Delete("Experimental", "PureDarkHUDFixMethod");
    }
    static double GetRefreshRate(HWND window);
    void RequestRuntimeInterpolation(bool enabled) { requested_.store(enabled, std::memory_order_release); }
    bool RuntimeInterpolationRequested() const { return requested_.load(std::memory_order_acquire); }
    double refreshRate{};

  private:
    SourceFrameGeneration() = default;
    std::atomic_bool requested_{true};
};
