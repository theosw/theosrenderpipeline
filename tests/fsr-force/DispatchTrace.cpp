#include "DispatchTrace.h"
#include "src/FrameGen/FSR4Compatibility.h"
#include <wrl/client.h>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace DispatchTrace
{
    using Microsoft::WRL::ComPtr;
    namespace
    {
        std::map<void**, void*> originals;
        std::map<void*, std::string> shaders;
        std::vector<ComPtr<ID3D12PipelineState>> retained;
        std::mutex mutex;
        std::ofstream psos, dispatches, submits;
        ID3D12GraphicsCommandList* observed{};
        ID3D12PipelineState* bound{};
        unsigned context{}, frame{}, sequence{};
        bool recording{}, submitted{}, available{true};
        struct Dispatch { std::string sha; UINT x, y, z; };
        std::vector<Dispatch> pending;
        template<class T> T Original(void* object, size_t slot)
        {
            return reinterpret_cast<T>(originals.at(*reinterpret_cast<void***>(object) + slot));
        }
        void Hook(void* object, size_t slot, void* replacement)
        {
            auto** entry = *reinterpret_cast<void***>(object) + slot;
            if (originals.contains(entry)) return;
            DWORD old{};
            if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) throw std::runtime_error("observer vtable protection");
            originals[entry] = *entry; *entry = replacement; DWORD ignored{};
            if (!VirtualProtect(entry, sizeof(void*), old, &ignored)) throw std::runtime_error("observer vtable restore");
        }
        void Register(const D3D12_SHADER_BYTECODE& code, REFIID iid, void* result, const char* route)
        {
            if (!result || iid != __uuidof(ID3D12PipelineState) || !code.pShaderBytecode || !code.BytecodeLength) { available = false; return; }
            const auto sha = TheosRenderPipeline::FSR4Compatibility::Sha256(code.pShaderBytecode, code.BytecodeLength);
            shaders[result] = sha; retained.emplace_back(static_cast<ID3D12PipelineState*>(result));
            psos << result << ',' << sha << ',' << code.BytecodeLength << ',' << route << '\n'; psos.flush();
        }
        HRESULT STDMETHODCALLTYPE Compute(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** result)
        {
            auto copy = *desc; copy.CachedPSO = {}; // Test deliberately bypasses supplied caches.
            const auto hr = Original<decltype(&Compute)>(self, 11)(self, &copy, iid, result);
            std::lock_guard lock(mutex);
            if (SUCCEEDED(hr) && result) Register(copy.CS, iid, *result, "CreateComputePipelineState");
            else psos << "creation_failed," << std::hex << hr << std::dec << ",0,compute\n";
            return hr;
        }
        template<class T> struct alignas(void*) Subobject { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; T value; };
        HRESULT STDMETHODCALLTYPE Stream(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid, void** result)
        {
            D3D12_SHADER_BYTECODE cs{}; size_t at = 0; bool understood = true;
            // Compute streams only. Any other path forwards untouched but makes
            // proof unavailable. The known AMD backend normally uses Compute.
            std::vector<std::byte> copy(desc->SizeInBytes);
            memcpy(copy.data(), desc->pPipelineStateSubobjectStream, copy.size());
            while (at < copy.size()) {
                if (copy.size() - at < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) { understood = false; break; }
                const auto type = *reinterpret_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(copy.data() + at);
                size_t size{};
                switch (type) {
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: size = sizeof(Subobject<ID3D12RootSignature*>); break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: size = sizeof(Subobject<D3D12_SHADER_BYTECODE>); break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: size = sizeof(Subobject<UINT>); break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: size = sizeof(Subobject<D3D12_PIPELINE_STATE_FLAGS>); break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: size = sizeof(Subobject<D3D12_CACHED_PIPELINE_STATE>); break;
                default: understood = false; break;
                }
                if (!understood || size > copy.size() - at) { understood = false; break; }
                if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) cs = reinterpret_cast<Subobject<D3D12_SHADER_BYTECODE>*>(copy.data() + at)->value;
                if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO) reinterpret_cast<Subobject<D3D12_CACHED_PIPELINE_STATE>*>(copy.data() + at)->value = {};
                at += size;
            }
            auto descriptor = *desc; if (understood) descriptor.pPipelineStateSubobjectStream = copy.data();
            const auto hr = Original<decltype(&Stream)>(self, 47)(self, &descriptor, iid, result);
            std::lock_guard lock(mutex);
            if (!understood) available = false;
            if (SUCCEEDED(hr) && result) Register(cs, iid, *result, "CreatePipelineState");
            return hr;
        }
        HRESULT STDMETHODCALLTYPE Library(ID3D12Device1*, const void*, SIZE_T, REFIID, void** result)
        {
            if (result) *result = nullptr;
            std::lock_guard lock(mutex); psos << "library_disabled,,0,CreatePipelineLibrary\n"; psos.flush();
            return E_NOTIMPL; // Explicitly disable unobservable pipeline-library/cache loads in this fixture.
        }
        void STDMETHODCALLTYPE Bind(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso)
        {
            if (self == observed) bound = pso;
            Original<decltype(&Bind)>(self, 25)(self, pso);
        }
        void STDMETHODCALLTYPE DispatchCall(ID3D12GraphicsCommandList* self, UINT x, UINT y, UINT z)
        {
            if (recording && self == observed && x && y && z) {
                std::lock_guard lock(mutex); const auto it = shaders.find(bound);
                if (it == shaders.end()) available = false;
                pending.push_back({it == shaders.end() ? "UNOBSERVED" : it->second, x, y, z});
            }
            Original<decltype(&DispatchCall)>(self, 14)(self, x, y, z);
        }
        void STDMETHODCALLTYPE Bundle(ID3D12GraphicsCommandList* self, ID3D12GraphicsCommandList* bundle)
        {
            if (recording && self == observed) available = false;
            Original<decltype(&Bundle)>(self, 27)(self, bundle);
        }
        void STDMETHODCALLTYPE Indirect(ID3D12GraphicsCommandList* self, ID3D12CommandSignature* signature, UINT count,
            ID3D12Resource* args, UINT64 offset, ID3D12Resource* counts, UINT64 countOffset)
        {
            if (recording && self == observed) available = false; // GPU argument contents not observed: cannot claim nonzero execution.
            Original<decltype(&Indirect)>(self, 59)(self, signature, count, args, offset, counts, countOffset);
        }
        void STDMETHODCALLTYPE Submit(ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists)
        {
            for (UINT i = 0; i < n; ++i) if (lists[i] == observed && !pending.empty()) {
                if (submitted) available = false;
                submitted = true;
            }
            Original<decltype(&Submit)>(self, 10)(self, n, lists);
        }
    }
    void Install(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue, const std::filesystem::path& out)
    {
        psos.open(out / "psos.csv"); dispatches.open(out / "dispatches.csv"); submits.open(out / "submissions.csv");
        psos << "pso,sha256,bytes,route\n"; dispatches << "context,frame,fence,ordinal,sha256,x,y,z\n";
        submits << "context,frame,fence,dispatches,observation_complete\n";
        observed = list;
        Hook(device, 11, reinterpret_cast<void*>(&Compute));
        ComPtr<ID3D12Device1> d1; ComPtr<ID3D12Device2> d2;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d1)))) { Hook(d1.Get(), 11, reinterpret_cast<void*>(&Compute)); Hook(d1.Get(), 44, reinterpret_cast<void*>(&Library)); }
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d2)))) {
            Hook(d2.Get(), 11, reinterpret_cast<void*>(&Compute)); Hook(d2.Get(), 44, reinterpret_cast<void*>(&Library)); Hook(d2.Get(), 47, reinterpret_cast<void*>(&Stream));
        }
        Hook(list, 25, reinterpret_cast<void*>(&Bind)); Hook(list, 14, reinterpret_cast<void*>(&DispatchCall));
        Hook(list, 27, reinterpret_cast<void*>(&Bundle)); Hook(list, 59, reinterpret_cast<void*>(&Indirect));
        Hook(queue, 10, reinterpret_cast<void*>(&Submit));
    }
    void Begin(unsigned c, unsigned f) { context = c; frame = f; pending.clear(); bound = nullptr; sequence = 0; submitted = false; recording = true; }
    void End() { recording = false; }
    void Complete(UINT64 fence)
    {
        if (!submitted || pending.empty()) available = false;
        submits << context << ',' << frame << ',' << fence << ',' << pending.size() << ',' << available << '\n';
        if (submitted) for (const auto& d : pending) dispatches << context << ',' << frame << ',' << fence << ',' << sequence++ << ',' << d.sha << ',' << d.x << ',' << d.y << ',' << d.z << '\n';
        submits.flush(); dispatches.flush(); pending.clear();
    }
    bool Available() { return available; }
    void Finish()
    {
        recording = false;
        for (auto [entry, original] : originals) {
            DWORD old{}, ignored{};
            if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) throw std::runtime_error("observer removal failed");
            *entry = original;
            if (!VirtualProtect(entry, sizeof(void*), old, &ignored)) throw std::runtime_error("observer removal protection failed");
        }
        originals.clear(); retained.clear(); psos.close(); dispatches.close(); submits.close();
    }
}
