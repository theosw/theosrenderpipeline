#include "NeuralAdmission.h"
#include "NeuralRuntimeKernels.h"
#include "NeuralRuntimeKernelsInternal.h"
#include "NeuralRenderingRuntimeIdentity.h"
#include "NvidiaDriverVersion.h"
#include <PCH.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <fstream>
#include <cstring>
#include <map>
#include <mutex>

namespace TheosRenderPipeline::NeuralRendering
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIAdapter1> Adapter(const LUID& luid)
        {
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter1> adapter;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
            }
            return adapter;
        }
        NeuralCapability Probe(const LUID& luid)
        {
            NeuralCapability c; c.luid = luid;
            auto adapter = Adapter(luid);
            DXGI_ADAPTER_DESC1 desc{};
            if (!adapter || FAILED(adapter->GetDesc1(&desc))) { return c; }
            c.nvidia = desc.VendorId == 0x10DE;
            c.dedicatedBytes = desc.DedicatedVideoMemory;
            if (!c.nvidia) { return c; }
            const auto cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (cuda) {
                const auto init = reinterpret_cast<int (WINAPI*)(unsigned)>(GetProcAddress(cuda, "cuInit"));
                const auto version = reinterpret_cast<int (WINAPI*)(int*)>(GetProcAddress(cuda, "cuDriverGetVersion"));
                const auto count = reinterpret_cast<int (WINAPI*)(int*)>(GetProcAddress(cuda, "cuDeviceGetCount"));
                const auto get = reinterpret_cast<int (WINAPI*)(int*, int)>(GetProcAddress(cuda, "cuDeviceGet"));
                const auto attribute = reinterpret_cast<int (WINAPI*)(int*, int, int)>(GetProcAddress(cuda, "cuDeviceGetAttribute"));
                const auto deviceLuid = reinterpret_cast<int (WINAPI*)(char*, unsigned*, int)>(GetProcAddress(cuda, "cuDeviceGetLuid"));
                int devices{};
                c.cudaReady = init && version && count && get && attribute && deviceLuid &&
                    version(&c.cudaDriver) == 0 && init(0) == 0 && count(&devices) == 0;
                for (int ordinal = 0; c.cudaReady && ordinal < devices; ++ordinal) {
                    int device{}, major{}, minor{}; char bytes[8]{}; unsigned mask{};
                    if (get(&device, ordinal) || deviceLuid(bytes, &mask, device) || !mask || std::memcmp(bytes, &luid, 8)) { continue; }
                    c.adapterMatched = true;
                    if (!attribute(&major, 75, device) && !attribute(&minor, 76, device)) { c.architecture = major * 10 + minor; }
                    if (attribute(&c.smCount, 16, device)) { c.smCount = 0; }
                    break;
                }
                FreeLibrary(cuda);
            }
            const auto nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (nvapi) {
                const auto query = reinterpret_cast<void* (__cdecl*)(unsigned)>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
                const auto initialize = query ? reinterpret_cast<int (__cdecl*)()>(query(0x0150E828)) : nullptr;
                const auto version = query ? reinterpret_cast<int (__cdecl*)(unsigned*, char*)>(query(0x2926AAAD)) : nullptr;
                char branch[64]{};
                if (initialize && initialize() == 0) {
                    if (version && version(&c.driverVersion, branch) == 0) { c.driverBranch = branch; c.driverSource = "NVAPI SYS_GetDriverAndBranchVersion"; }
                    c.nvapiReady = query(0xad1a677d) && query(0xe2436e22) && query(0xdf295ea6) &&
                        query(0x41c65285) && query(0x24973538) && query(0x846a9bf0);
                }
                FreeLibrary(nvapi);
            }
            // Documented fallback: rendering adapter's DXGI UMD version, using the
            // existing NVIDIA Windows version encoding (32.0.15.8278 -> 58278).
            // CUDA API version is never used as a display-driver version.
            if (!c.driverVersion) {
                LARGE_INTEGER version{};
                if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) {
                    c.driverVersion = SourceDLSSG::NvidiaDriverVersion(version.LowPart);
                    c.driverSource = "DXGI rendering-adapter UMD version fallback";
                }
            }
            return c;
        }
    }
    NeuralCapability QueryNeuralHardware(const LUID& luid, bool universal)
    {
        static std::mutex mutex;
        static std::map<std::pair<LONG, DWORD>, NeuralCapability> cache;
        std::scoped_lock lock(mutex);
        const auto key = std::pair{luid.HighPart, luid.LowPart};
        auto it = cache.find(key);
        if (it == cache.end()) { it = cache.emplace(key, Probe(luid)).first; }
        auto c = it->second; c.universal = universal; return c;
    }
    bool VerifyPascalPack(const std::filesystem::path& packPath)
    {
        if (!NeuralRenderingRuntimeIdentity::VerifyExpected(packPath, kPascalPackBytes, kPascalPackSha256, true).matched) { return false; }
        std::ifstream file(packPath, std::ios::binary);
        std::vector<std::uint8_t> bytes(kPascalPackBytes);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) { return false; }
        std::vector<KernelPacks::Patch> plan; std::string reason;
        return KernelPacks::MakePlan(bytes, 0x100000000ull, 0x110000000ull, plan, reason, bytes.size(), true);
    }
    bool VerifyPascalAssets(const std::filesystem::path& runtime, bool freshRuntime)
    {
        return NeuralRenderingRuntimeIdentity::VerifyExpected(runtime, 266424944, kKernelRuntimeSha256, freshRuntime).matched &&
            VerifyPascalPack(runtime.parent_path() / kPascalPackName);
    }
    NeuralCapability QueryNeuralAdmission(const LUID& luid, const std::filesystem::path& runtime,
        unsigned width, unsigned height, unsigned passes, bool universal, bool log)
    {
        auto c = QueryNeuralHardware(luid, universal);
        if (!c.Pascal()) { return c; }
        c.estimateBytes = EstimatePascalMemory(width, height, passes);
        // Standard never needs to inspect or install compatibility bytes.
        if (universal) { c.assetsVerified = VerifyPascalAssets(runtime, log); }
        auto adapter = Adapter(luid); ComPtr<IDXGIAdapter3> budget;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (adapter && SUCCEEDED(adapter.As(&budget)) &&
            SUCCEEDED(budget->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            c.budgetKnown = true; c.budgetBytes = info.Budget; c.usageBytes = info.CurrentUsage;
        }
        if (log) {
            logger::info("[NR Admission] LUID={:08X}:{:08X} SM={} count={} dedicated={} driver={} branch={} source={} CUDA={} init={} match={} NVAPI={} pack={} budget={} usage={} estimatedNeed={} status={}",
                static_cast<unsigned>(luid.HighPart), luid.LowPart, c.architecture, c.smCount, c.dedicatedBytes,
                c.driverVersion, c.driverBranch, c.driverSource, c.cudaDriver, c.cudaReady, c.adapterMatched,
                c.nvapiReady, c.assetsVerified, c.budgetBytes, c.usageBytes, c.estimateBytes, c.Refusal() ? c.Refusal() : kPascalStatus);
        }
        return c;
    }
}
