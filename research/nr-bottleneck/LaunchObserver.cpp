#include "LaunchObserver.h"
#include "ReusePlan.h"
#include "FrameGen/NeuralRenderingRuntimeIdentity.h"
#include "FrameGen/NeuralRenderingRuntimeContract.h"
#include <d3d12.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // ABI: NVIDIA/nvapi 87dca625, nvapi.h, NVAPI_CU_KERNEL_LAUNCH_PARAMS.
    // These experimental interfaces are deliberately confined to this probe.
    struct Dim { unsigned x, y, z; };
    struct Launch { void* function; Dim grid, block; unsigned shared; const void* params; unsigned size; };
    static_assert(offsetof(Launch, params) == 40 && offsetof(Launch, size) == 48 && sizeof(Launch) == 56);
    using Resolver = FARPROC(WINAPI*)(HMODULE, LPCSTR);
    using Query = void*(__cdecl*)(unsigned);
    using Create = int(__cdecl*)(ID3D12Device*, void*, const char*, void**);
    using Execute = int(__cdecl*)(ID3D12GraphicsCommandList*, const Launch*, unsigned);
    struct Sample { std::string name; Launch launch; std::vector<unsigned char> bytes; };
    struct Observer
    {
        std::mutex mutex;
        Resolver resolver{}; Query query{}; Create create{}; Execute execute{};
        HMODULE nvapi{};
        std::unordered_map<void*, std::string> names;
        std::vector<Sample> samples;
        Bottleneck::ReusePlan plan;
        std::atomic<bool> failed{ false };
        bool active{};
        DWORD thread{};
        unsigned frame{};
    };
    Observer* observer{}; // One private oracle, one observer. Lifetime extends through NGX shutdown.

    int __cdecl CreateFunction(ID3D12Device* device, void* module, const char* name, void** result)
    {
        auto& s = *observer;
        const auto status = s.create(device, module, name, result);
        if (status == 0 && name && result) {
            try { std::lock_guard lock(s.mutex); s.names[*result] = name; }
            catch (...) { s.failed = true; }
        }
        return status;
    }

    int __cdecl ExecuteKernels(ID3D12GraphicsCommandList* list, const Launch* kernels, unsigned count)
    {
        auto& s = *observer;
        std::vector<Launch> kept;
        bool filtered = false;
        try {
            std::lock_guard lock(s.mutex);
            if (s.active) {
                if (s.thread != GetCurrentThreadId() || !kernels || count == 0 || count > 256)
                    s.failed = true;
                else for (unsigned i = 0; i < count; ++i) {
                    const auto& k = kernels[i];
                    auto name = s.names.find(k.function);
                    if (name == s.names.end() || !k.params || k.size > 4096) { s.failed = true; break; }
                    Sample sample{ name->second, k, {} };
                    auto* data = static_cast<const unsigned char*>(k.params);
                    sample.bytes.assign(data, data + k.size);
                    const bool drop = s.plan.Observe({sample.name,
                        {k.grid.x, k.grid.y, k.grid.z, k.block.x, k.block.y, k.block.z, k.shared}, sample.bytes});
                    if (drop) filtered = true;
                    else kept.push_back(k);
                    s.samples.push_back(std::move(sample));
                }
            }
        } catch (...) { s.failed = true; }
        if (s.failed) return -1; // The oracle rejects this command list without executing it.
        if (filtered) return kept.empty() ? 0 : s.execute(list, kept.data(), static_cast<unsigned>(kept.size()));
        return s.execute(list, kernels, count);
    }

    void* __cdecl QueryInterface(unsigned id)
    {
        auto& s = *observer;
        auto* result = s.query(id);
        if (!result) return result;
        if (id == 0xe2436e22) {
            if (s.create && s.create != reinterpret_cast<Create>(result)) { s.failed = true; return result; }
            s.create = reinterpret_cast<Create>(result); return reinterpret_cast<void*>(&CreateFunction);
        }
        if (id == 0x24973538) {
            if (s.execute && s.execute != reinterpret_cast<Execute>(result)) { s.failed = true; return result; }
            s.execute = reinterpret_cast<Execute>(result); return reinterpret_cast<void*>(&ExecuteKernels);
        }
        return result;
    }

    FARPROC WINAPI Resolve(HMODULE module, LPCSTR name)
    {
        auto& s = *observer;
        const auto result = s.resolver(module, name);
        if (module == s.nvapi && reinterpret_cast<std::uintptr_t>(name) > 65535 &&
            std::strcmp(name, "nvapi_QueryInterface") == 0) {
            if (reinterpret_cast<void*>(result) != reinterpret_cast<void*>(s.query)) { s.failed = true; return result; }
            return reinterpret_cast<FARPROC>(&QueryInterface);
        }
        return result;
    }

    void WritePointer(void** slot, void* value)
    {
        DWORD old{}, ignored{};
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) throw std::runtime_error("Import protection failed");
        InterlockedExchangePointer(slot, value);
        if (!VirtualProtect(slot, sizeof(void*), old, &ignored)) throw std::runtime_error("Import protection restore failed");
    }
}

struct LaunchObserver::State
{
    Observer observer;
    HMODULE runtime{};
    void** slot{};
    void* original{};
    std::ofstream trace;
};

LaunchObserver::LaunchObserver(const std::filesystem::path& runtime, const std::filesystem::path& output) :
    state_(std::make_unique<State>())
{
    namespace Identity = TheosRenderPipeline::NeuralRenderingRuntimeIdentity;
    namespace NR = TheosRenderPipeline::NeuralRendering;
    if (observer) throw std::runtime_error("Only one observer is supported in this standalone process");
    const auto identity = Identity::VerifyExpected(runtime, NR::kRuntimeSize, NR::kNexusRuntimeSha256);
    if (!identity.matched) throw std::runtime_error("Observer rejects unrecognized NR runtime");
    auto& s = *state_;
    s.runtime = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!s.runtime || !Identity::EqualPath(Identity::ModulePath(s.runtime), identity.path))
        throw std::runtime_error("Observer runtime load/path mismatch");
    auto& o = s.observer;
    o.nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!o.nvapi) throw std::runtime_error("NVAPI missing");
    o.query = reinterpret_cast<Query>(GetProcAddress(o.nvapi, "nvapi_QueryInterface"));
    if (!o.query) throw std::runtime_error("NVAPI query missing");
    auto* base = reinterpret_cast<std::byte*>(s.runtime);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; entry->Name; ++entry) {
        if (!entry->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            if (std::strcmp(name, "GetProcAddress")) continue;
            if (s.slot) throw std::runtime_error("Ambiguous resolver import");
            s.slot = reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    if (!s.slot) throw std::runtime_error("NR resolver import missing");
    s.original = *s.slot;
    // Reject pre-existing instrumentation in this private process rather than guessing its ownership.
    if (s.original != reinterpret_cast<void*>(&GetProcAddress)) throw std::runtime_error("NR resolver already wrapped");
    o.resolver = reinterpret_cast<Resolver>(s.original);
    s.trace.open(output / "launches.tsv");
    if (!s.trace) throw std::runtime_error("Cannot create launch trace");
    s.trace << "frame\tindex\tdropped\tname\tgrid\tblock\tshared\tparams_hex\n";
    observer = &o;
    try { WritePointer(s.slot, reinterpret_cast<void*>(&Resolve)); }
    catch (...) {
        // WritePointer may have exchanged the slot before protection restoration
        // failed. Retain callback state even on this terminal constructor path.
        state_.release();
        throw;
    }
}

LaunchObserver::~LaunchObserver()
{
    // The oracle has completed all GPU work and destroyed its features before reaching here.
    // Keep the explicitly loaded modules until process exit: they may cache our callback pointers.
    if (state_->slot && *state_->slot == reinterpret_cast<void*>(&Resolve)) {
        try { WritePointer(state_->slot, state_->original); } catch (...) { std::terminate(); }
    }
    // NVAPI query results can remain cached until DLL process detach. Keep the
    // callback state alive as well as the modules, including during CRT teardown.
    // This is bounded to one observer per short-lived research process.
    state_.release();
}

void LaunchObserver::Begin(unsigned frame, bool reset, bool reuse)
{
    auto& o = state_->observer;
    std::lock_guard lock(o.mutex);
    if (o.active || o.failed) throw std::runtime_error("Observer is not ready");
    o.plan.Begin(reset, reuse);
    o.samples.clear(); o.frame = frame; o.thread = GetCurrentThreadId(); o.active = true;
}

void LaunchObserver::End()
{
    auto& o = state_->observer;
    std::lock_guard lock(o.mutex);
    o.active = false;
    if (o.failed || o.samples.empty()) throw std::runtime_error("Incomplete NR launch observation; command list must not be submitted");
    o.plan.End();
    unsigned index{};
    for (const auto& sample : o.samples) {
        const auto& k = sample.launch;
        auto& out = state_->trace;
        const bool dropped = o.plan.Reusing() && index >= o.plan.First() && index <= o.plan.Last();
        out << std::dec << o.frame << '\t' << index++ << '\t' << int(dropped) << '\t' << sample.name << '\t'
            << k.grid.x << ',' << k.grid.y << ',' << k.grid.z << '\t'
            << k.block.x << ',' << k.block.y << ',' << k.block.z << '\t' << k.shared << '\t';
        for (const auto byte : sample.bytes) out << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
        out << '\n';
    }
    state_->trace.flush();
    if (!state_->trace) throw std::runtime_error("Launch trace write failed");
}

void LaunchObserver::Retired() { state_->observer.plan.Retired(); }
bool LaunchObserver::Reusing() const { return state_->observer.plan.Reusing(); }
