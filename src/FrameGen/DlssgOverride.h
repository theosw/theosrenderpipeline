#pragma once

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <Windows.h>
#include "../PluginPaths.h"

namespace TheosRenderPipeline::SourceDLSSG
{
    // NVIDIA app's DLSS Frame Generation overrides (model preset or DLL version)
    // make NGX load its downloaded DLSS-G from its model store instead of the
    // copy in TRP's Streamline folder, for example
    // C:\ProgramData\NVIDIA\NGX\models\dlssg\versions\<n>\files\160_E658700.bin.
    inline bool IsNvidiaModelStorePath(const std::filesystem::path& path)
    {
        auto text = path.generic_wstring();
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return text.find(L"/nvidia/ngx/models/") != std::wstring::npos;
    }

    // The DLSS-G NGX loaded from NVIDIA's model store because of such an override,
    // retained; null without one. Shared by both editions.
    inline HMODULE RetainNvidiaOverrideDlssg()
    {
        // Normalize only modules already under a model store path (or with an
        // 8.3 short name): normalizing all ~500 modules is slow under MO2.
        const auto candidate = [](const std::filesystem::path& raw) {
            return IsNvidiaModelStorePath(raw) || raw.native().find(L'~') != std::wstring::npos;
        };
        return PluginPaths::RetainLoadedModuleIf(candidate, [](const std::filesystem::path& path, HMODULE module) {
            auto text = path.generic_wstring();
            std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return text.find(L"/nvidia/ngx/models/") != std::wstring::npos &&
                text.find(L"/models/dlssg/") != std::wstring::npos &&
                ::GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature") != nullptr;
        });
    }
}
