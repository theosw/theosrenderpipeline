#include "NeuralAdmission.h"
#include "NeuralRuntimeKernels.h"
#include "NeuralRuntimeKernelsInternal.h"
#include "NeuralRenderingRuntimeIdentity.h"
#include "NvidiaDriverVersion.h"
#include <PCH.h>
#include "GpuMemoryStatistics.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <fstream>
#include <cstring>
#include <map>
#include <mutex>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>

namespace TheosRenderPipeline::NeuralRendering
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        std::atomic_uint64_t hardwareProbes{}, packProbes{}, budgetQueries{};
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
            ++hardwareProbes;
            NeuralCapability c; c.luid = luid;
            auto adapter = Adapter(luid);
            DXGI_ADAPTER_DESC1 desc{};
            if (!adapter || FAILED(adapter->GetDesc1(&desc))) { return c; }
            c.nvidia = desc.VendorId == 0x10DE;
            c.dedicatedBytes = desc.DedicatedVideoMemory;
            c.deviceId = desc.DeviceId;
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
    namespace
    {
        // A file of another length can never match a pinned digest, so it is refused
        // without hashing (the menu lists every installed NR runtime, 150-300 MB each).
        bool SizeIs(const std::filesystem::path& path, std::uint64_t bytes)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            return !error && size == bytes;
        }
    }
    bool VerifyPascalPack(const std::filesystem::path& packPath, bool fresh)
    {
        if (!SizeIs(packPath, kPascalPackBytes)) { return false; }
        if (!NeuralRenderingRuntimeIdentity::VerifyExpected(packPath, kPascalPackBytes, kPascalPackSha256, fresh).matched) { return false; }
        // A pinned SHA identifies every byte of the manifest as well. Once the
        // parser accepts that exact content it need not allocate/parse per poll.
        static std::mutex mutex;
        static bool manifestVerified{};
        std::scoped_lock lock(mutex);
        if (manifestVerified && !fresh) { return true; }
        ++packProbes;
        std::ifstream file(packPath, std::ios::binary);
        std::vector<std::uint8_t> bytes(kPascalPackBytes);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) { return false; }
        std::vector<KernelPacks::Patch> plan; std::string reason;
        return manifestVerified = KernelPacks::MakePlan(bytes, 0x100000000ull, 0x110000000ull, plan, reason, bytes.size(), true);
    }
    bool VerifyPascalAssets(const std::filesystem::path& runtime, bool freshRuntime)
    {
        if (!SizeIs(runtime, kPascalRuntimeBytes)) { return false; }
        return NeuralRenderingRuntimeIdentity::VerifyExpected(runtime, kPascalRuntimeBytes, kKernelRuntimeSha256, freshRuntime).matched &&
            VerifyPascalPack(runtime.parent_path() / kPascalPackName, freshRuntime);
    }
    std::optional<bool> PollPascalAssets(const std::filesystem::path& runtime)
    {
        if (!SizeIs(runtime, kPascalRuntimeBytes) || !SizeIs(runtime.parent_path() / kPascalPackName, kPascalPackBytes)) { return false; }
        // Hashing the original runtime takes about a second on GTX 10-era CPUs and
        // disks; on the render thread that froze the first menu frame.
        struct Check { std::atomic_bool done{}; };
        static std::mutex mutex;
        static std::map<std::wstring, std::shared_ptr<Check>> checks;
        std::shared_ptr<Check> check;
        {
            std::scoped_lock lock(mutex);
            auto& entry = checks[runtime.wstring()];
            if (!entry) {
                entry = std::make_shared<Check>();
                // Detached: process exit must never wait for a hash.
                std::thread([started = entry, runtime] {
                    try { VerifyPascalAssets(runtime, false); } catch (...) {}
                    started->done = true;
                }).detach();
                return std::nullopt;
            }
            check = entry;
        }
        if (!check->done) { return std::nullopt; }
        // The identity cache answers now; only a replaced file is hashed again.
        return VerifyPascalAssets(runtime, false);
    }
    void QueryNeuralBudget(NeuralCapability& c)
    {
        ++budgetQueries;
        c.budgetKnown = false; c.budgetBytes = c.usageBytes = 0;
        // Retain the actual LUID's interface; periodic polls only make the DXGI
        // budget call, never recreate a factory or enumerate hardware.
        static std::mutex mutex;
        static std::map<std::pair<LONG, DWORD>, ComPtr<IDXGIAdapter3>> adapters;
        std::scoped_lock lock(mutex);
        auto& budget = adapters[{c.luid.HighPart, c.luid.LowPart}];
        if (!budget) {
            auto adapter = Adapter(c.luid);
            if (adapter) { adapter.As(&budget); }
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (budget && SUCCEEDED(budget->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            c.budgetKnown = true; c.budgetBytes = info.Budget; c.usageBytes = info.CurrentUsage;
        }
        const auto kernel = GpuMemory::Query(c.luid);
        c.adapterKnown = kernel.has_value();
        c.adapterFreeBytes = kernel ? kernel->AdapterFree() : 0;
        c.processCommittedBytes = kernel ? kernel->processCommitted : 0;
    }
    AdmissionMetrics GetAdmissionMetrics() { return {hardwareProbes.load(), packProbes.load(), budgetQueries.load()}; }
    NeuralCapability QueryNeuralAdmission(const LUID& luid, const std::filesystem::path& runtime,
        unsigned width, unsigned height, unsigned passes, bool universal, bool log, std::uint64_t retainedEstimate, bool freshAssets,
        bool pollAssets)
    {
        auto c = QueryNeuralHardware(luid, universal);
        if (!c.Pascal()) { return c; }
        c.extentPending = !width || !height;
        if (c.extentPending) {
            if (log) { logger::info("[NR Admission] pending scene extent {}x{}; asset verification and budget query deferred", width, height); }
            return c;
        }
        c.estimateBytes = EstimatePascalMemory(width, height, passes);
        c.retainedEstimateBytes = retainedEstimate;
        // Standard never needs to inspect or install compatibility bytes.
        if (universal && pollAssets) {
            const auto verified = PollPascalAssets(runtime);
            c.assetsPending = !verified;
            c.assetsVerified = verified.value_or(false);
        } else if (universal) {
            c.assetsVerified = VerifyPascalAssets(runtime, freshAssets);
        }
        QueryNeuralBudget(c);
        if (log) {
            logger::info("[NR Admission] LUID={:08X}:{:08X} SM={} count={} dedicated={} driver={} branch={} source={} CUDA={} init={} match={} NVAPI={} pack={} budget={} usage={} processCommitted={} cardFree={} estimatedNeed={} retainedEstimate={} status={}",
                static_cast<unsigned>(luid.HighPart), luid.LowPart, c.architecture, c.smCount, c.dedicatedBytes,
                c.driverVersion, c.driverBranch, c.driverSource, c.cudaDriver, c.cudaReady, c.adapterMatched,
                c.nvapiReady, c.assetsVerified, c.budgetBytes, c.usageBytes,
                c.adapterKnown ? std::to_string(c.processCommittedBytes) : std::string("unknown"),
                c.adapterKnown ? std::to_string(c.adapterFreeBytes) : std::string("unknown"),
                c.estimateBytes, c.retainedEstimateBytes, c.Refusal() ? c.Refusal() : kPascalStatus);
        }
        return c;
    }
}
