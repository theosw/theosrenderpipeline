#include <PCH.h>
#include "StartupHints.h"
#include "CommunityShaderIntegration.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "PluginPaths.h"
#include "Localization.h"
#include "UserSettings.h"
#include <SimpleIni.h>

namespace TheosRenderPipeline::StartupHints
{
    namespace
    {
        // HwSchMode: 2 = on, 1 = off. A missing value is the Windows default
        // and is not reported as off.
        bool HardwareSchedulingOff()
        {
            DWORD mode = 0, size = sizeof(mode);
            return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                L"HwSchMode", RRF_RT_REG_DWORD, nullptr, &mode, &size) == ERROR_SUCCESS && mode == 1;
        }

        // "<module> already loaded by another owner": report where that copy lives.
        std::string OtherOwner(std::string_view status)
        {
            constexpr std::string_view marker = " already loaded by another owner";
            const auto end = status.find(marker);
            if (end == std::string_view::npos) { return {}; }
            const auto begin = status.rfind(' ', end == 0 ? 0 : end - 1);
            const std::filesystem::path name(status.substr(begin == std::string_view::npos ? 0 : begin + 1,
                end - (begin == std::string_view::npos ? 0 : begin + 1)));
            const auto module = GetModuleHandleW(name.c_str());
            return module ? PluginPaths::ModulePath(module).string() : std::string{};
        }
    }

    Facts CurrentFacts(std::string_view status)
    {
        const auto& backend = SourceDLSSG::Backend::Get();
        Facts facts;
        facts.universal = Plugin::EDITION == "Universal";
        facts.communityShaders = CommunityShaders::Active();
        facts.frameGenerationUnsupportedReported = backend.FrameGenerationUnsupportedReported();
        facts.frameGenerationOverrideReported = backend.FrameGenerationOverrideReported();
        facts.hardwareSchedulingOff = HardwareSchedulingOff();
        try { facts.otherOwner = OtherOwner(status); } catch (...) {}
        return facts;
    }

    std::string DialogHintFor(std::string_view status, const Facts& facts)
    {
        // Device creation can fail before OverlayUI activates the dictionary.
        // Do not initialize rendering/UI or read/write CS settings to show help.
        if (facts.communityShaders && Classify(status) == Failure::SwapchainNotAcquired) {
            try {
                CSimpleIniA ini;
                ini.SetUnicode();
                UserSettings::Load(ini);
                Localization::Activate(ini.GetValue("Overlay", "Language", "auto"));
                return Hint(Failure::SwapchainNotAcquired, facts,
                    [](const char* text) { return Localization::Translate(text); });
            } catch (...) {
                // An unavailable dictionary must never hide the recovery steps.
            }
        }
        return HintFor(status, facts);
    }
}
