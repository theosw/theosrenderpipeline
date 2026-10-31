#include "NeuralLaunchHook.h"
#include "NeuralLaunchSubmission.h"
#include "NeuralFaultInjection.h"
#include "NeuralRuntimeKernels.h"
#include <PCH.h>
#include <cstring>
#include <atomic>

namespace TheosRenderPipeline::NeuralRendering
{
    namespace
    {
        using Query = void* (__cdecl*)(std::uint32_t);
        using Create = int (__cdecl*)(ID3D12Device*, void*, const char*, void**);
        using CreateModule = int (__cdecl*)(ID3D12Device*, const void*, std::uint32_t, void**);
        using Destroy = int (__cdecl*)(ID3D12Device*, void*);
        using Chain = int (__cdecl*)(ID3D12GraphicsCommandList*, const KernelLaunch*, std::uint32_t);
        using ChainEx = int (__cdecl*)(ID3D12GraphicsCommandList*, const KernelLaunchEx*, std::uint32_t);
        constexpr std::uint32_t moduleCreateId=0xad1a677d, createId=0xe2436e22, destroyId=0xdf295ea6, moduleDestroyId=0x41c65285,
            chainId=0x24973538, chainExId=0x846a9bf0;
        struct Hook {
            HMODULE runtime{}, nvapi{};
            LaunchBindings bindings;
            Query query{};
            std::atomic<Create> create{};
            std::atomic<CreateModule> createModule{};
            std::atomic<Destroy> destroy{}, destroyModule{};
            std::atomic<Chain> chain{};
            std::atomic<ChainEx> chainEx{};
            FARPROC (WINAPI* getProc)(HMODULE,LPCSTR){};
            std::map<void*,void*> modules;
            std::map<void*,int> bundles;
            std::set<std::string> logged;
            bool resolved{};
            bool unsafe{};
            std::mutex mutex;
        };
        // No static destructor: the DLL and callbacks remain valid at process exit.
        Hook& Active() { static auto* hook = new Hook; return *hook; }
        int __cdecl CreateModuleHandle(ID3D12Device* device, const void* blob, std::uint32_t bytes, void** module)
        {
            auto& h=Active();
            const auto bundle=RuntimeKernelBundle(h.runtime,blob,bytes);
            const auto result=h.createModule.load()(device,blob,bytes,module);
            if (!result && module && *module) { std::scoped_lock lock(h.mutex); h.bundles[*module]=bundle; }
            return result;
        }
        int __cdecl CreateFunction(ID3D12Device* device, void* module, const char* name, void** function)
        {
            auto& h=Active();
            {
                std::scoped_lock lock(h.mutex);
                const auto rule=name?h.bindings.requirements.find(name):h.bindings.requirements.end();
                if (rule!=h.bindings.requirements.end() &&
                    (!h.bundles.contains(module) || h.bundles.at(module)!=static_cast<int>(rule->second.bundle))) {
                    h.bindings.failure=std::string("NR required kernel resolved in an unexpected bundle: ")+name;
                    return -1;
                }
            }
            const auto result=h.create.load()(device,module,name,function);
            std::scoped_lock lock(h.mutex);
            if (!result && function && *function) {
                h.bindings.names[*function]=name ? name : "";
                h.modules[*function]=module;
            }
            return result;
        }
        int __cdecl DestroyFunction(ID3D12Device* device, void* function)
        {
            auto& h=Active();
            const auto result=h.destroy.load()(device,function);
            if (!result) { std::scoped_lock lock(h.mutex); h.bindings.names.erase(function); h.modules.erase(function); }
            return result;
        }
        int __cdecl DestroyModule(ID3D12Device* device, void* module)
        {
            auto& h=Active();
            const auto result=h.destroyModule.load()(device,module);
            if (!result) {
                std::scoped_lock lock(h.mutex);
                h.bundles.erase(module);
                for (auto it=h.modules.begin(); it!=h.modules.end();) {
                    if (it->second==module) { h.bindings.names.erase(it->first); it=h.modules.erase(it); } else { ++it; }
                }
            }
            return result;
        }
        template<class T, class F> int Launch(ID3D12GraphicsCommandList* list, const T* kernels, std::uint32_t count, F forward)
        {
            auto& h=Active();
            std::scoped_lock lock(h.mutex);
            // NVIDIA fail-fasts on NVAPI errors. Unwind Evaluate normally, then
            // FeatureSession observes the latched failure before submission.
            if (!h.bindings.failure.empty()) { RejectedNeuralLists::Get().Reject(list); return 0; }
            std::vector<T> rewritten;
            if (!h.bindings.Rewrite(kernels,count,rewritten)) {
                RejectedNeuralLists::Get().Reject(list);
                logger::error("[DLSSNR Launch] {}; NR off",h.bindings.failure); return 0;
            }
            for (std::uint32_t i=0;i<count;++i) {
                const auto& name=h.bindings.names.at(kernels[i].function);
                if (h.bindings.requirements.contains(name) && h.logged.insert(name).second) {
                    const auto& r=rewritten[i];
                    logger::info("[DLSSNR Launch] {} block={}x{}x{} -> {}x{}x{}; grid={}x{}x{} -> {}x{}x{}; chain kept whole ({} kernels)",name,
                        kernels[i].block.x,kernels[i].block.y,kernels[i].block.z,r.block.x,r.block.y,r.block.z,
                        kernels[i].grid.x,kernels[i].grid.y,kernels[i].grid.z,r.grid.x,r.grid.y,r.grid.z,count);
                }
            }
            if (FaultInjection::Hit(FaultInjection::Point::Chain)) { h.bindings.failure="injected NR chain refusal"; RejectedNeuralLists::Get().Reject(list); return 0; }
            const auto result=FaultInjection::Hit(FaultInjection::Point::NVAPI) ? -1 : forward(list,rewritten.empty()?kernels:rewritten.data(),count);
            if (result && count) { h.bindings.failure=std::format("NVAPI refused required NR launch geometry (status={}, count={})",result,count); logger::error("[DLSSNR Launch] {}",h.bindings.failure); RejectedNeuralLists::Get().Reject(list); return 0; }
            return result;
        }
        int __cdecl LaunchChain(ID3D12GraphicsCommandList* l,const KernelLaunch* k,std::uint32_t n) { return Launch(l,k,n,Active().chain.load()); }
        int __cdecl LaunchChainEx(ID3D12GraphicsCommandList* l,const KernelLaunchEx* k,std::uint32_t n) { return Launch(l,k,n,Active().chainEx.load()); }
        void* __cdecl QueryInterface(std::uint32_t id)
        {
            auto& h=Active();
            auto* original=h.query(id);
            if (!original) { return nullptr; }
            switch(id) {
            case moduleCreateId: h.createModule=reinterpret_cast<CreateModule>(original); return reinterpret_cast<void*>(&CreateModuleHandle);
            case createId: h.create=reinterpret_cast<Create>(original); return reinterpret_cast<void*>(&CreateFunction);
            case destroyId: h.destroy=reinterpret_cast<Destroy>(original); return reinterpret_cast<void*>(&DestroyFunction);
            case moduleDestroyId: h.destroyModule=reinterpret_cast<Destroy>(original); return reinterpret_cast<void*>(&DestroyModule);
            case chainId: h.chain=reinterpret_cast<Chain>(original); return reinterpret_cast<void*>(&LaunchChain);
            case chainExId: h.chainEx=reinterpret_cast<ChainEx>(original); return reinterpret_cast<void*>(&LaunchChainEx);
            default: return original;
            }
        }
        FARPROC WINAPI GetProc(HMODULE module,LPCSTR name)
        {
            auto& h=Active();
            const auto result=h.getProc(module,name);
            if (result && reinterpret_cast<std::uintptr_t>(name)>0xFFFF && !std::strcmp(name,"nvapi_QueryInterface")) {
                std::scoped_lock lock(h.mutex);
                if (module!=h.nvapi) { h.bindings.failure="NR resolved an unexpected NVAPI module"; return nullptr; }
                h.resolved=true;
                return reinterpret_cast<FARPROC>(&QueryInterface);
            }
            return result;
        }
    }
    bool InstallLaunchHook(HMODULE runtime,const LaunchRequirements& requirements,int capability,std::string& reason)
    {
        if (requirements.empty()) { return true; }
        auto& h=Active();
        std::scoped_lock lock(h.mutex);
        for (const auto& [name,r]:requirements) {
            const auto bit=capability==61?1u:capability==89?2u:0u;
            if (!(r.architectures & bit)) { reason="NR launch requirements do not cover this GPU architecture"; return false; }
        }
        if (h.runtime) {
            if (h.runtime!=runtime || h.bindings.requirements!=requirements || !h.bindings.failure.empty()) {
                reason="NR required-launch hook conflicts with retained runtime; restart required"; return false;
            }
            return true;
        }
        h.nvapi=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        h.query=reinterpret_cast<Query>(::GetProcAddress(h.nvapi,"nvapi_QueryInterface"));
        if (!h.query) { reason="NR required-launch hook: NVAPI resolver missing"; return false; }
        h.create=reinterpret_cast<Create>(h.query(createId));
        h.createModule=reinterpret_cast<CreateModule>(h.query(moduleCreateId));
        h.destroy=reinterpret_cast<Destroy>(h.query(destroyId));
        h.destroyModule=reinterpret_cast<Destroy>(h.query(moduleDestroyId));
        h.chain=reinterpret_cast<Chain>(h.query(chainId));
        h.chainEx=reinterpret_cast<ChainEx>(h.query(chainExId));
        if (!h.createModule || !h.create || !h.destroy || !h.destroyModule || !h.chain || !h.chainEx) {
            reason="NR required-launch hook: Create/Destroy/Chain/ChainEx unavailable; NR off"; return false;
        }
        auto* base=reinterpret_cast<std::uint8_t*>(runtime);
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        const auto& dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) { reason="NR required-launch hook: import table missing"; return false; }
        for (auto* entry=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);entry->Name;++entry) {
            if (!entry->OriginalFirstThunk) { continue; }
            auto* names=reinterpret_cast<const IMAGE_THUNK_DATA64*>(base+entry->OriginalFirstThunk);
            auto* iat=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+entry->FirstThunk);
            for (;names->u1.AddressOfData;++names,++iat) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) { continue; }
                const auto* byName=reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(byName->Name),"GetProcAddress")) { continue; }
                DWORD old{},ignored{};
                if (!VirtualProtect(&iat->u1.Function,sizeof(void*),PAGE_READWRITE,&old)) { reason="NR resolver import is not writable"; return false; }
                h.getProc=reinterpret_cast<decltype(h.getProc)>(iat->u1.Function);
                h.bindings.requirements=requirements;
                h.bindings.capability=capability;
                h.runtime=runtime;
                InterlockedExchangePointer(reinterpret_cast<void**>(&iat->u1.Function),reinterpret_cast<void*>(&GetProc));
                if (!VirtualProtect(&iat->u1.Function,sizeof(void*),old,&ignored)) {
                    h.unsafe=true;
                    h.bindings.failure=reason="NR resolver import protection restoration failed; NR off"; return false;
                }
                logger::info("[DLSSNR Launch] installed required geometry for {} kernels; Chain and ChainEx covered",requirements.size());
                return true;
            }
        }
        reason="NR required-launch hook: GetProcAddress import missing; NR off";
        return false;
    }
    bool LaunchHookRecoverySafe()
    {
        auto& h=Active(); std::scoped_lock lock(h.mutex); return !h.unsafe;
    }
    bool ResetLaunchHookAfterRetirement()
    {
        auto& h=Active(); std::scoped_lock lock(h.mutex);
        if (h.unsafe) { return false; }
        h.bindings.failure.clear();
        // Keep cached function/module identities: some runtimes cache NVAPI
        // handles process-wide. Reinstallation still verifies the same contract.
        return true;
    }
    bool LaunchHookHealthy(HMODULE runtime,std::string& reason)
    {
        auto& h=Active(); std::scoped_lock lock(h.mutex);
        if (h.runtime!=runtime) { return true; }
        if (!h.bindings.failure.empty()) { reason=h.bindings.failure; return false; }
        return true;
    }
    bool LaunchHookReady(HMODULE runtime,std::string& reason)
    {
        auto& h=Active(); std::scoped_lock lock(h.mutex);
        if (h.runtime!=runtime) { return true; }
        if (!h.resolved || !h.bindings.Ready()) {
            reason=h.bindings.failure.empty()?"NR required-launch hook: unresolved kernel name or resolver bypass; NR off":h.bindings.failure;
            for (const auto& [name, rule]:h.bindings.requirements) {
                bool found=false;
                for (const auto& [handle,bound]:h.bindings.names) { found |= bound==name; }
                if (!found && rule.block.x) { reason += "; missing "+name; }
            }
            return false;
        }
        return true;
    }
}
