#pragma once
#include "FrameGen/SourceDLSSGSession.h"
#include <sl.h>

// Uses official Streamline exports and the production Session. No patching,
// injection, NR or game hooks. NVIDIA phases remain passthrough in this fixture.
struct NvidiaPresenterFixture {
    SessionAPI api;
    Session session;
    ComPtr<ID3D12Device> proxyDevice;
    ComPtr<IDXGIFactory> proxyFactory;
    bool started{};
    PFun_slFreeResources* freeResources{};
    static void SL(sl::Result result, const char* stage)
    {
        std::printf("SL stage=%s result=%u\n", stage, static_cast<unsigned>(result));
        Require(result == sl::Result::eOk, stage);
    }
    NvidiaPresenterFixture(
        const std::filesystem::path& directory, ID3D12Device* device, IDXGIFactory* factory, Interop& interop)
    {
        auto module = Load(directory / L"sl.interposer.dll");
        auto init = Export<PFun_slInit*>(module, "slInit");
        auto setDevice = Export<PFun_slSetD3DDevice*>(module, "slSetD3DDevice");
        auto upgrade = Export<PFun_slUpgradeInterface*>(module, "slUpgradeInterface");
        auto supported = Export<PFun_slIsFeatureSupported*>(module, "slIsFeatureSupported");
        auto function = Export<PFun_slGetFeatureFunction*>(module, "slGetFeatureFunction");
        api.newFrameToken = Export<PFun_slGetNewFrameToken*>(module, "slGetNewFrameToken");
        api.setConstants = Export<PFun_slSetConstants*>(module, "slSetConstants");
        api.setTag = Export<PFun_slSetTag*>(module, "slSetTag");
        freeResources = Export<PFun_slFreeResources*>(module, "slFreeResources");
        const wchar_t* paths[]{directory.c_str()};
        const sl::Feature features[]{sl::kFeatureReflex, sl::kFeatureDLSS_G};
        sl::Preferences prefs{};
        prefs.pathsToPlugins = paths;
        prefs.numPathsToPlugins = 1;
        prefs.featuresToLoad = features;
        prefs.numFeaturesToLoad = 2;
        prefs.renderAPI = sl::RenderAPI::eD3D12;
        prefs.engine = sl::EngineType::eCustom;
        prefs.engineVersion = "TRP XeFG presenter fixture";
        prefs.projectId = "f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11";
        prefs.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
            sl::PreferenceFlags::eUseDXGIFactoryProxy;
        prefs.logMessageCallback = [](sl::LogType, const char* message) { std::printf("[SL] %s\n", message); };
        SL(init(prefs, sl::kSDKVersion), "init");
        void* upgraded = device;
        device->AddRef();
        SL(upgrade(&upgraded), "upgrade device");
        proxyDevice.Attach(static_cast<ID3D12Device*>(upgraded));
        SL(setDevice(device), "device");
        auto luid = device->GetAdapterLuid();
        sl::AdapterInfo info{};
        info.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
        info.deviceLUIDSizeInBytes = sizeof(luid);
        for (auto feature : {sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS_G})
            SL(supported(feature, info), "support");
        auto resolve = [&](sl::Feature feature, const char* name, auto& target) {
            void* p{};
            SL(function(feature, name, p), name);
            Require(p != nullptr, name);
            target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(p);
        };
        resolve(sl::kFeatureReflex, "slReflexSetOptions", api.setReflexOptions);
        resolve(sl::kFeatureReflex, "slReflexSleep", api.reflexSleep);
        resolve(sl::kFeatureReflex, "slReflexGetState", api.getReflexState);
        resolve(sl::kFeaturePCL, "slPCLSetMarker", api.marker);
        resolve(sl::kFeatureDLSS_G, "slDLSSGGetState", api.getState);
        resolve(sl::kFeatureDLSS_G, "slDLSSGSetOptions", api.setOptions);
        api.context = &interop;
        api.waitForInputReaders = [](void* context, void* fence, uint64_t value) {
            return SUCCEEDED(
                static_cast<Interop*>(context)->WaitForInputReaders(static_cast<ID3D12Fence*>(fence), value));
        };
        upgraded = factory;
        factory->AddRef();
        SL(upgrade(&upgraded), "upgrade factory");
        proxyFactory.Attach(static_cast<IDXGIFactory*>(upgraded));
    }
    void Begin(std::uint32_t lastApplicationFrame = 0)
    {
        Require(started ? session.ResumeAfterResize(lastApplicationFrame) : session.Start(api, 1),
            "production NVIDIA session start/resume");
        started = true;
    }
    void BeforePresent() { Require(session.BeforePresent(false), "NVIDIA passthrough preparation"); }
    void AfterPresent(HRESULT result)
    {
        Require(session.AfterPresent(SUCCEEDED(result), false, true), "NVIDIA suspend and retire Reflex");
    }
    void ReleaseRetiredResources()
    {
        SL(freeResources(sl::kFeatureDLSS_G, sl::ViewportHandle(1)), "release retired NVIDIA FG resources");
    }
};
