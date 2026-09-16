#include "SourceFrameGeneration.h"
#include <PCH.h>
#include <SimpleIni.h>

void SourceFrameGeneration::LoadINI()
{
    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile(L"Data\\SKSE\\Plugins\\TheosRenderPipeline.ini");
    LoadStartupPreferences(ini);
    logger::info("[NvidiaHost] required; startup interpolation={} UI composition mode={}", settings.enabled, settings.nativeUICompositionMode);
}

double SourceFrameGeneration::GetRefreshRate(HWND a_window)
{
    const auto monitor = ::MonitorFromWindow(a_window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!::GetMonitorInfoW(monitor, &monitorInfo))
    {
        return 0.0;
    }
    DEVMODEW devMode{};
    devMode.dmSize = sizeof(devMode);
    if (!::EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode))
    {
        return 0.0;
    }
    return static_cast<double>(devMode.dmDisplayFrequency);
}
