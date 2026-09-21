#include "NeuralRenderingBottleneck.h"
#include "NeuralRenderingBottleneckPlan.h"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace TheosRenderPipeline::NeuralRendering
{
    namespace
    {
        // NVIDIA/nvapi 87dca625, NVAPI_CU_KERNEL_LAUNCH_PARAMS. Install is called
        // only after FeatureSession has verified the exact NR 310.8 binary.
        struct Dim { unsigned x, y, z; };
        struct Launch { void* function; Dim grid, block; unsigned shared; const void* params; unsigned size; };
        static_assert(sizeof(Launch) == 56 && offsetof(Launch, params) == 40 && offsetof(Launch, size) == 48);
        using Resolver = FARPROC(WINAPI*)(HMODULE, LPCSTR);
        using Query = void*(__cdecl*)(unsigned);
        using Create = int(__cdecl*)(ID3D12Device*, void*, const char*, void**);
        using Execute = int(__cdecl*)(ID3D12GraphicsCommandList*, const Launch*, unsigned);
        struct Bridge
        {
            std::mutex mutex;
            HMODULE runtime{}, nvapi{}, callbacks{};
            Resolver resolver{};
            Query query{};
            Create create{};
            Execute execute{};
            bool installed{}, attempted{};
            std::atomic<bool> unavailable{};
            std::unordered_map<void*, std::string> names;
        };
        // One process-lifetime bridge, not one leak per feature. NVAPI/runtime
        // cache these callbacks through DLL detach; never leave dangling state.
        // All three modules are pinned. Feature histories remain normal RAII.
        Bridge& Global() { static auto* bridge = new Bridge; return *bridge; }
        struct Recording
        {
            Bottleneck::ReusePlan plan;
            ID3D12GraphicsCommandList* list{};
            unsigned observed{}, dropped{};
            bool disabled{}, fault{}, pending{}, reused{};
            std::string status{ "warming up" };
        };
        thread_local Recording* active{};

        int __cdecl CreateFunction(ID3D12Device* device, void* module, const char* name, void** result)
        {
            auto& b = Global();
            const int code = b.create(device, module, name, result);
            if (code == 0 && name && result && *result) {
                try {
                    std::lock_guard lock(b.mutex);
                    if (b.names.size() >= 4096 && !b.names.contains(*result)) b.unavailable = true;
                    else b.names[*result] = name;
                } catch (...) { b.unavailable = true; }
            }
            return code;
        }
        int __cdecl ExecuteKernels(ID3D12GraphicsCommandList* list, const Launch* kernels, unsigned count)
        {
            auto& b = Global();
            auto* r = active;
            if (!r || r->disabled) return b.execute(list, kernels, count);
            try {
                if (b.unavailable || list != r->list || !kernels || count == 0 || count > 256)
                    throw std::runtime_error("unrecognized kernel caller");
                std::vector<Launch> kept;
                kept.reserve(count);
                for (unsigned i = 0; i < count; ++i) {
                    const auto& k = kernels[i];
                    std::string name;
                    {
                        std::lock_guard lock(b.mutex);
                        const auto found = b.names.find(k.function);
                        if (found == b.names.end()) throw std::runtime_error("unobserved kernel creation");
                        name = found->second;
                    }
                    if (!k.params || k.size > 4096) throw std::runtime_error("unrecognized kernel parameters");
                    const auto* data = static_cast<const unsigned char*>(k.params);
                    const bool drop = r->plan.Observe({std::move(name),
                        {k.grid.x, k.grid.y, k.grid.z, k.block.x, k.block.y, k.block.z, k.shared},
                        std::vector<unsigned char>(data, data + k.size)});
                    ++r->observed;
                    if (drop) ++r->dropped;
                    else kept.push_back(k);
                }
                if (kept.size() == count) return b.execute(list, kernels, count);
                return kept.empty() ? 0 : b.execute(list, kept.data(), static_cast<unsigned>(kept.size()));
            } catch (...) {
                // Before any omission this whole API call can safely pass
                // through. After an omission the host must reject the list.
                r->disabled = true;
                r->fault = r->dropped != 0;
                return r->fault ? -1 : b.execute(list, kernels, count);
            }
        }
        void* __cdecl QueryInterface(unsigned id)
        {
            auto& b = Global();
            void* result = b.query(id);
            if (id == 0xe2436e22 && result == reinterpret_cast<void*>(b.create)) return reinterpret_cast<void*>(&CreateFunction);
            if (id == 0x24973538 && result == reinterpret_cast<void*>(b.execute)) return reinterpret_cast<void*>(&ExecuteKernels);
            return result;
        }
        FARPROC WINAPI Resolve(HMODULE module, LPCSTR name)
        {
            auto& b = Global();
            const auto result = b.resolver(module, name);
            if (module == b.nvapi && reinterpret_cast<std::uintptr_t>(name) > 65535 &&
                std::strcmp(name, "nvapi_QueryInterface") == 0 && result == reinterpret_cast<FARPROC>(b.query))
                return reinterpret_cast<FARPROC>(&QueryInterface);
            return result;
        }
    }
    struct BottleneckReuse::State { Recording recording; };
    BottleneckReuse::BottleneckReuse() : state_(std::make_unique<State>()) {}
    BottleneckReuse::~BottleneckReuse() = default;

    bool BottleneckReuse::Install(HMODULE runtime)
    {
        auto& b = Global();
        std::lock_guard lock(b.mutex);
        if (b.installed) return b.runtime == runtime && !b.unavailable;
        if (!runtime || b.unavailable || b.attempted) return false;
        b.attempted = true;
        b.nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!b.nvapi) return false;
        b.query = reinterpret_cast<Query>(GetProcAddress(b.nvapi, "nvapi_QueryInterface"));
        if (!b.query) return false;
        b.create = reinterpret_cast<Create>(b.query(0xe2436e22));
        b.execute = reinterpret_cast<Execute>(b.query(0x24973538));
        if (!b.create || !b.execute) return false;
        auto* base = reinterpret_cast<std::byte*>(runtime);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        void** slot{};
        if (!dir.VirtualAddress) return false;
        for (auto* entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); entry->Name; ++entry) {
            if (!entry->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
                if (std::strcmp(name, "GetProcAddress")) continue;
                if (slot) return false;
                slot = reinterpret_cast<void**>(&slots->u1.Function);
            }
        }
        // Preserve foreign instrumentation. An unsupported bridge means normal
        // NR, never replacement of another module's import owner.
        if (!slot || *slot != reinterpret_cast<void*>(&GetProcAddress)) return false;
        constexpr DWORD pin = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
        if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(runtime), &b.runtime) ||
            !GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(&Resolve), &b.callbacks)) return false;
        HMODULE pinnedNvapi{};
        if (!GetModuleHandleExW(pin, reinterpret_cast<LPCWSTR>(b.nvapi), &pinnedNvapi)) return false;
        b.resolver = &GetProcAddress;
        DWORD old{}, ignored{};
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
        const auto previous = InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(&Resolve), reinterpret_cast<void*>(&GetProcAddress));
        const bool restored = VirtualProtect(slot, sizeof(void*), old, &ignored) != 0;
        b.installed = previous == reinterpret_cast<void*>(&GetProcAddress);
        b.unavailable = !restored;
        return b.installed && restored;
    }
    bool BottleneckReuse::Begin(ID3D12GraphicsCommandList* list, bool reset)
    {
        auto& r = state_->recording;
        r.reused = false;
        if (r.disabled) return true;
        if (active || r.pending || !list) { r.status = "invalid evaluation/submission ordering"; return false; }
        try {
            r.plan.Begin(reset, true);
            r.list = list; r.observed = r.dropped = 0; r.fault = false;
            active = &r;
            return true;
        } catch (...) { r.status = "could not begin bottleneck observation"; return false; }
    }
    bool BottleneckReuse::End(bool succeeded)
    {
        auto& r = state_->recording;
        if (active == &r) active = nullptr;
        if (!succeeded) { r.plan.Failed(); r.status = "vendor evaluation failed"; return false; }
        if (!r.disabled) {
            try { r.plan.End(); r.pending = true; }
            catch (...) { r.disabled = true; r.fault = r.dropped != 0; }
        }
        if (r.disabled) {
            r.plan.Failed();
            r.status = r.fault ? "kernel contract changed after omission; list rejected" : "unsupported kernel contract; ordinary NR";
            return !r.fault;
        }
        r.reused = r.dropped == 42;
        r.status = r.reused ? "reused 42 coarse-stage kernels" : "full evaluation";
        return true;
    }
    void BottleneckReuse::Submitted()
    {
        auto& r = state_->recording;
        if (!r.pending) return;
        r.plan.Submitted();
        r.pending = false;
    }
    bool BottleneckReuse::Reused() const { return state_->recording.reused; }
    const std::string& BottleneckReuse::Status() const { return state_->recording.status; }
}
