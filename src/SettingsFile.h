#pragma once

#include <SimpleIni.h>
#include <cerrno>
#include <cstdio>
#include <memory>

namespace TheosRenderPipeline::SettingsFile
{
inline SI_Error LoadForUpdate(CSimpleIniA& ini, const wchar_t* path)
{
    FILE* file = nullptr;
    const auto error = _wfopen_s(&file, path, L"rb");
    if (error != 0)
    {
        // A missing file may be created. An unreadable file must be preserved.
        return error == ENOENT ? SI_OK : SI_FILE;
    }
    const std::unique_ptr<FILE, decltype(&std::fclose)> input(file, &std::fclose);
    return ini.LoadFile(input.get());
}
} // namespace TheosRenderPipeline::SettingsFile
