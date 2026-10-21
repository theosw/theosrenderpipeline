#pragma once
#include <Windows.h>
#include "../PluginPaths.h"

namespace TheosRenderPipeline::NeuralRendering
{
    struct ResearchConfiguration { wchar_t mode{}; std::filesystem::path pack; };
    inline ResearchConfiguration ReadPascalConfiguration(const std::filesystem::path& ini, const std::filesystem::path& runtimeFolder)
    {
            ResearchConfiguration result;
            (void)ini; (void)runtimeFolder;
#if defined(TRP_ENABLE_PASCAL_RESEARCH) || defined(TRP_NR_VENDOR_BENCHMARK)
            result.mode=GetPrivateProfileIntW(L"NeuralRenderingResearch",L"EnablePascal",0,ini.c_str())==1?L'1':L'\0';
            wchar_t mode[8]{};
            const auto n=GetEnvironmentVariableW(L"TRP_NR_PASCAL_RESEARCH",mode,8);
            if (n) { result.mode=n==1 && (mode[0]==L'1' || mode[0]==L'2')?mode[0]:L'\0'; }
            wchar_t pack[32768]{};
            const auto length=GetEnvironmentVariableW(L"TRP_NR_PASCAL_PACK",pack,32768);
            if (!length) { GetPrivateProfileStringW(L"NeuralRenderingResearch",L"PascalPack",L"",pack,32768,ini.c_str()); }
            if (length < 32768 && pack[0]) {
                result.pack=pack;
                if (result.pack.is_relative()) { result.pack=runtimeFolder/result.pack; }
            }
#endif
            return result;
    }
    inline const ResearchConfiguration& PascalConfiguration()
    {
        static const auto config=ReadPascalConfiguration(PluginPaths::Directory()/L"TheosRenderPipeline.Diagnostics.ini",
            PluginPaths::Directory()/L"TheosRenderPipeline");
        return config;
    }
    inline bool PascalResearchFor(int capability)
    {
        if (!PascalConfiguration().mode) { return false; }
#if defined(TRP_NR_VENDOR_BENCHMARK)
        return capability==61 || capability==89; // explicit reference testing on the daily GPU
#else
        return capability==61;
#endif
    }
}
