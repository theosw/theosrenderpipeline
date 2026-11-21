#include "NvidiaAppProfile.h"
#include "PluginPaths.h"

#include <memory>
#include <Windows.h>

namespace TheosRenderPipeline::NvidiaApp
{
    Values ReadProfile(const std::wstring& exeName)
    {
        // NvAPI DRS through nvapi_QueryInterface; the public SDK is not a TRP dependency.
        struct Setting
        {
            std::uint32_t version;
            wchar_t name[2048];
            std::uint32_t id, type, location, isCurrentPredefined, isPredefinedValid;
            union Value { std::uint32_t u32; std::uint8_t binary[4100]; wchar_t text[2048]; } predefined, current;
        };
        struct Application
        {
            std::uint32_t version, isPredefined;
            wchar_t appName[2048], friendlyName[2048], launcher[2048], fileInFolder[2048];
            std::uint32_t flags;
            wchar_t commandLine[2048];
        };
        Values values;
        const auto nvapi = GetModuleHandleW(L"nvapi64.dll") ? GetModuleHandleW(L"nvapi64.dll") : LoadLibraryW(L"nvapi64.dll");
        const auto query = nvapi ? reinterpret_cast<void* (*)(std::uint32_t)>(GetProcAddress(nvapi, "nvapi_QueryInterface")) : nullptr;
        if (!query) { return values; }
        using Init = int (*)();
        using Create = int (*)(void**);
        using Load = int (*)(void*);
        using FindApp = int (*)(void*, const wchar_t*, void**, Application*);
        using Base = int (*)(void*, void**);
        using Get = int (*)(void*, void*, std::uint32_t, Setting*);
        using Destroy = int (*)(void*);
        const auto init = reinterpret_cast<Init>(query(0x0150E828));
        const auto create = reinterpret_cast<Create>(query(0x0694D52E));
        const auto load = reinterpret_cast<Load>(query(0x375DBD6B));
        const auto findApp = reinterpret_cast<FindApp>(query(0xEEE566B2));
        const auto base = reinterpret_cast<Base>(query(0xDA8466A0));
        const auto get = reinterpret_cast<Get>(query(0x73BF8338));
        const auto destroy = reinterpret_cast<Destroy>(query(0xDAD9CFF8));
        if (!init || !create || !load || !findApp || !base || !get || !destroy || init() != 0) { return values; }
        void* session{};
        if (create(&session) != 0) { return values; }
        if (load(session) == 0) {
            auto setting = std::make_unique<Setting>();
            auto application = std::make_unique<Application>();
            void* game{};
            void* global{};
            const std::wstring exe = exeName.empty() ? PluginPaths::ModulePath(GetModuleHandleW(nullptr)).filename().wstring() : exeName;
            application->version = sizeof(Application) | (4u << 16);
            if (findApp(session, exe.c_str(), &game, application.get()) != 0) { game = nullptr; }
            if (base(session, &global) != 0) { global = nullptr; }
            const auto read = [&](std::uint32_t id) -> std::optional<std::uint32_t> {
                for (void* profile : {game, global}) {
                    *setting = {};
                    setting->version = sizeof(Setting) | (1u << 16);
                    if (profile && get(session, profile, id, setting.get()) == 0 && setting->type == 0) {
                        return setting->current.u32;
                    }
                }
                return std::nullopt;
            };
            values.read = game || global;
            values.upscalingEnable = read(kUpscalingEnable);
            values.upscalingPreset = read(kUpscalingPreset);
            values.upscalingForceDLAA = read(kUpscalingForceDLAA);
            values.generationEnable = read(kGenerationEnable);
            values.generationPreset = read(kGenerationPreset);
            values.generationMode = read(kGenerationMode);
            values.generationFrames = read(kGenerationFrames);
            values.neuralEnable = read(kNeuralEnable);
            values.neuralPreset = read(kNeuralPreset);
            values.streamlineEnable = read(kStreamlineEnable);
        }
        destroy(session);
        return values;
    }

    bool SmoothMotionLoaded() { return GetModuleHandleW(L"NvPresent64.dll") != nullptr; }
}
