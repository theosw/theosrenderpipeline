#include "runtime.hpp"
#include "module_loader.hpp"
#include "module_patch.hpp"
#include "provider_retarget.hpp"
#include "../RTX40MFG/midpoint_fix.h"
#include "../RTX40MFG/dlssg_provider_policy.h"
#include "../../src/FrameGen/SourceDLSSGMFGPatch.h"
#include <nvsdk_ngx.h>
#include <dxgi.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdio>
#include <exception>

namespace trp::ampere {
namespace {
// Minimal public NVAPI ABI from NVIDIA/nvapi 87dca625e83fd89a983e19b904e5f3a580da90d2.
// NV_GPU_ARCH_INFO V1/V2 have the same four 32-bit fields. Query IDs and the
// capability policy follow MFGAmpereUnlock-RenoDx; see LICENSE and THIRD-PARTY.
struct ArchInfo { std::uint32_t version, architecture, implementation, revision; };
static_assert(sizeof(ArchInfo) == 16);
using Query = void* (__cdecl*)(std::uint32_t);
using GetArch = int (__cdecl*)(void*, ArchInfo*);
using EnumGPUs = int (__cdecl*)(void**, std::uint32_t*);
using GetLuid = int (__cdecl*)(void*, void*);
using GetName = int (__cdecl*)(void*, char*);
using Initialize = int (__cdecl*)();
using Resolver = FARPROC (WINAPI*)(HMODULE, LPCSTR);
using Requirements = decltype(&NVSDK_NGX_D3D12_GetFeatureRequirements);
using Parameters = decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters);
using Create = decltype(&NVSDK_NGX_D3D12_CreateFeature);
// NVIDIA public NVAPI ABI: nvapi.h / nvapi_interface.h, CreateCuModule.
using CreateModule = int (__cdecl*)(ID3D12Device*, const void*, std::uint32_t, void**);
constexpr std::uint32_t kAmpere = 0x170, kAda = 0x190, kTuring = 0x160;
bool IsRTX20(std::string_view name) {
    // SM75 also includes GTX 16; do not expose RTX features based on SM alone.
    if (name.starts_with("NVIDIA ")) name.remove_prefix(7);
    for (const auto model : {"GeForce RTX 2060", "GeForce RTX 2070", "GeForce RTX 2080"}) {
        const std::string_view prefix(model);
        if (name.starts_with(prefix) && (name.size()==prefix.size() || name[prefix.size()]==' ')) return true;
    }
    return false;
}
struct Owner {
    std::atomic_bool started{}, prepared{}, installed{}, failed{}, createSeen{};
    std::atomic<const char*> error{};
    std::atomic_uint32_t mask{}, requirementsCalls{}, capabilityCalls{}, createCalls{};
    std::uint32_t fatbins{}, targetSm{86}, nativeArchitecture{kAmpere};
    Log log{};
    Fatal fatal{};
    LUID luid{};
    void* gpu{};
    HMODULE provider{}, common{}, wrapper{}, nvapi{}, core{};
    Query query{}; GetArch arch{};
    std::array<Resolver, 3> resolvers{};
    std::array<void**, 3> importSlots{};
    std::atomic<CreateModule> createModule{};
    std::atomic_uint32_t moduleCalls{};
    struct Program { const void* address; std::size_t size; };
    std::vector<Program> programs;
    std::atomic<Requirements> requirements{};
    std::array<std::atomic<Parameters>, 2> parameters{};
    std::atomic<Create> create{};
    std::mutex resolverMutex;
    memory::Transaction data, imports;
    midpoint_fix::AmpereTemporalClone temporal;
    std::uint8_t* minimumArch{};
};
Owner& State() { static auto* state = new Owner; return *state; }
thread_local unsigned startupScope{};
thread_local bool suppressExposure{};
void Message(const char* text) noexcept { try { if (State().log) State().log(text); } catch (...) {} }
bool Fail(const char* reason) noexcept {
    auto& s = State(); const char* empty{};
    s.error.compare_exchange_strong(empty, reason); s.failed.store(true);
    Message(reason); return false;
}
struct ExposureScope {
    bool previous;
    explicit ExposureScope(bool expose) : previous(suppressExposure) { suppressExposure = !expose; ++startupScope; }
    ~ExposureScope() { --startupScope; suppressExposure = previous; }
};
bool SameAdapter(IDXGIAdapter* adapter) noexcept {
    DXGI_ADAPTER_DESC desc{};
    return adapter && SUCCEEDED(adapter->GetDesc(&desc)) && desc.VendorId == 0x10de &&
        std::memcmp(&desc.AdapterLuid, &State().luid, sizeof(LUID)) == 0;
}
bool Prepared() noexcept { auto& s = State(); return s.prepared.load() && s.installed.load() && !s.failed.load(); }
int __cdecl Architecture(void* gpu, ArchInfo* info) {
    auto& s = State();
    const int result = s.arch(gpu, info);
    if (result == 0 && info && (info->version == 0x10010 || info->version == 0x20010) &&
        startupScope && !suppressExposure && gpu == s.gpu && Prepared()) info->architecture = kAda;
    return result;
}
void* __cdecl QueryInterface(std::uint32_t id) {
    auto& s = State(); auto* result = s.query(id);
    if (id == 0xd8265d24 && result == reinterpret_cast<void*>(s.arch)) return reinterpret_cast<void*>(Architecture);
    return result;
}
bool BlobFingerprint(const void* blob,std::uint32_t size,std::uint64_t& hash) noexcept {
    if(size>fatbin::kMaxFatbinBytes)return false;
    __try {
        hash=14695981039346656037ull;
        const auto* bytes=static_cast<const std::uint8_t*>(blob);
        for(std::uint32_t i=0;i<size;++i){hash^=bytes[i];hash*=1099511628211ull;}
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){hash=0;return false;}
}
int __cdecl CreateCuModule(ID3D12Device* device, const void* blob, std::uint32_t size, void** output) {
    auto& s=State();const auto real=s.createModule.load();
    if(!real)return -3; // NVAPI_NO_IMPLEMENTATION
    const auto result=real(device,blob,size,output);
    // 310.9.1 calls (nullptr,nullptr,0,nullptr) to probe API presence. Its
    // expected error must neither poison startup nor count as a kernel load.
    if(!device || !blob || !size || !output)return result;
    const auto call=++s.moduleCalls;
    if(result==0 && *output)return result;
    int program=-1;
    for(std::size_t i=0;i<s.programs.size();++i)if(s.programs[i].address==blob){program=static_cast<int>(i);break;}
    std::array<std::uint8_t,80> header{};
    const bool readable=size>=header.size() && memory::Copy(header.data(),blob,header.size());
    std::uint64_t fingerprint{};const bool hashed=BlobFingerprint(blob,size,fingerprint);
    char detail[512]{};
    std::snprintf(detail,sizeof(detail),
        "stage=cu-module-load target=SM%u call=%u program=%d bytes=%u status=%d handle=%s headerReadable=%s magic=%08x payloadBytes=%llu firstKind=%u firstSM=%u firstCompressedBytes=%u firstFlags=%llx firstUnpackedBytes=%llu fingerprintValid=%s blobFNV1a64=%016llx",
        s.targetSm,call,program,size,result,*output?"non-null":"null",readable?"true":"false",
        fatbin::ReadU32(header.data()),static_cast<unsigned long long>(fatbin::ReadU64(header.data()+8)),
        static_cast<unsigned>(fatbin::ReadU16(header.data()+16)),fatbin::ReadU32(header.data()+44),fatbin::ReadU32(header.data()+32),
        static_cast<unsigned long long>(fatbin::ReadU64(header.data()+56)),static_cast<unsigned long long>(fatbin::ReadU64(header.data()+72)),hashed?"true":"false",static_cast<unsigned long long>(fingerprint));
    Message(detail);
    constexpr auto reason="RTX20 frame-generation kernel loading failed. NVIDIA did not create a usable GPU module; restart required. See stage=cu-module-load in TheosRenderPipeline.log.";
    Fail(reason);
    // Never unwind into NVIDIA or let it continue after this failure. A late
    // Streamline log callback cannot provide this boundary. Modules/resources
    // stay resident until process exit; there is no unsafe recovery/cleanup.
    if(s.fatal)s.fatal(reason);
    std::terminate(); // Host callback is required to terminate, never return.
}
void* __cdecl ProviderQueryInterface(std::uint32_t id) {
    auto& s=State();auto* result=s.query(id);
    if(id!=0xad1a677d || !result)return result;
    auto typed=reinterpret_cast<CreateModule>(result);CreateModule empty{};
    if(!s.createModule.compare_exchange_strong(empty,typed) && empty!=typed) {
        Fail("provider CuModule export changed identity; restart required");
        if(s.fatal)s.fatal(s.error.load());
        std::terminate();
    }
    // Provider architecture queries deliberately pass through unchanged.
    return reinterpret_cast<void*>(CreateCuModule);
}
NVSDK_NGX_Result NVSDK_CONV GetRequirements(IDXGIAdapter* adapter,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery, NVSDK_NGX_FeatureRequirement* output) {
    auto& s = State(); const auto real = s.requirements.load();
    if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const bool fg = discovery && discovery->FeatureID == NVSDK_NGX_Feature_FrameGeneration;
    const bool bound = fg && SameAdapter(adapter);
    ExposureScope scope(bound);
    const auto result = real(adapter, discovery, output);
    if (!fg) return result;
    ++s.requirementsCalls;
    // Preserve driver/OS/other flags and the original call failure. This is
    // limited to this prepared provider on the host's actual rendering adapter.
    if (bound && Prepared() && result == NVSDK_NGX_Result_Success && output) {
        const auto flags = static_cast<std::uint32_t>(output->FeatureSupported);
        if ((flags == 0 || flags == 4) && (output->MinHWArchitecture == kAda || (flags == 4 && (output->MinHWArchitecture == kAmpere || output->MinHWArchitecture == s.nativeArchitecture)))) {
            output->MinHWArchitecture = s.nativeArchitecture;
            output->FeatureSupported = static_cast<NVSDK_NGX_Feature_Support_Result>(0);
        }
    }
    return result;
}
void UpdateCapabilities(NVSDK_NGX_Parameter* parameters) {
    if (!parameters || !Prepared()) return;
    int available{}, maximum{};
    const auto haveAvailable = parameters->Get("FrameGeneration.Available", &available);
    const auto haveMaximum = parameters->Get("DLSSG.MultiFrameCountMax", &maximum);
    if (haveAvailable == NVSDK_NGX_Result_Success && available == 0) {
        int needsDriver = 1; unsigned featureResult = NVSDK_NGX_Result_Success;
        const auto driver = parameters->Get("FrameGeneration.NeedsUpdatedDriver", &needsDriver);
        const auto init = parameters->Get("FrameGeneration.FeatureInitResult", &featureResult);
        const bool initOk = init != NVSDK_NGX_Result_Success || featureResult == NVSDK_NGX_Result_Success || featureResult == NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        if (driver == NVSDK_NGX_Result_Success && needsDriver == 0 && initOk) {
            parameters->Set("FrameGeneration.Available", 1);
            if (parameters->Get("FrameGeneration.Available", &available) != NVSDK_NGX_Result_Success || available != 1) Fail("NGX did not retain Ampere availability");
        }
    }
    if (haveAvailable == NVSDK_NGX_Result_Success && available > 0 && haveMaximum == NVSDK_NGX_Result_Success && maximum >= 0 && maximum < 5) {
        parameters->Set("DLSSG.MultiFrameCountMax", 5);
        if (parameters->Get("DLSSG.MultiFrameCountMax", &maximum) != NVSDK_NGX_Result_Success || maximum != 5) Fail("NGX did not retain Ampere MFG capacity");
    }
}
template<unsigned I> NVSDK_NGX_Result NVSDK_CONV GetParameters(NVSDK_NGX_Parameter** output) {
    auto& s = State(); const auto real = s.parameters[I].load();
    if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    ExposureScope scope(true);
    const auto result = real(output); ++s.capabilityCalls;
    if (result == NVSDK_NGX_Result_Success && output) UpdateCapabilities(*output);
    return result;
}
NVSDK_NGX_Result NVSDK_CONV CreateFeature(ID3D12GraphicsCommandList* commands, NVSDK_NGX_Feature feature,
    NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
    auto& s = State(); const auto real = s.create.load();
    if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const bool fg = feature == NVSDK_NGX_Feature_FrameGeneration;
    ExposureScope scope(fg);
    if (fg) {
        // Preparation is immutable from this point, before the vendor can
        // register/cache/submit anything that references the transformed data.
        s.createSeen.store(true); ++s.createCalls;
        if (!Verify()) { if (handle) *handle = nullptr; return NVSDK_NGX_Result_FAIL_FeatureNotSupported; }
    }
    const auto result = real(commands, feature, parameters, handle);
    if(fg && s.failed.load()) {
        if(handle)*handle=nullptr;
        Message("NGX frame-generation creation refused after an internal startup failure");
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    if (fg) Message(result == NVSDK_NGX_Result_Success ? "Ampere NGX frame-generation feature created" : "Ampere NGX frame-generation creation failed");
    return result;
}
static_assert(std::is_same_v<decltype(&CreateFeature), Create>);
bool IsCore(HMODULE module, FARPROC address) {
    wchar_t path[32768]{};
    const auto count = GetModuleFileNameW(module, path, 32768);
    if (!count || count >= 32768) return false;
    const auto name = std::filesystem::path(path).filename().wstring();
    if (_wcsicmp(name.c_str(), L"_nvngx.dll") && _wcsicmp(name.c_str(), L"nvngx.dll")) return false;
    memory::Image image;
    return image.Open(module) && image.OwnCode(reinterpret_cast<void*>(address));
}
template<class T> bool Bind(std::atomic<T>& storage, FARPROC address) {
    const auto typed = reinterpret_cast<T>(address); T empty{};
    return storage.compare_exchange_strong(empty, typed) || empty == typed;
}
template<unsigned I> FARPROC WINAPI Resolve(HMODULE module, LPCSTR name) {
    auto& s = State(); auto result = s.resolvers[I](module, name);
    if (!result || reinterpret_cast<std::uintptr_t>(name) <= 65535) return result;
    try {
        if (module == s.nvapi && std::strcmp(name, "nvapi_QueryInterface") == 0 && reinterpret_cast<void*>(result) == reinterpret_cast<void*>(s.query)) {
            s.mask.fetch_or(1u << (4 + I*5));
            if constexpr(I==2)return reinterpret_cast<FARPROC>(ProviderQueryInterface);
            return reinterpret_cast<FARPROC>(QueryInterface);
        }
        if constexpr (I != 0) return result;
        constexpr const char* names[]{"NVSDK_NGX_D3D12_GetFeatureRequirements", "NVSDK_NGX_D3D12_GetCapabilityParameters", "NVSDK_NGX_D3D12_GetParameters", "NVSDK_NGX_D3D12_CreateFeature"};
        unsigned index = 4;
        for (unsigned n = 0; n < 4; ++n) if (std::strcmp(name, names[n]) == 0) { index=n; break; }
        if (index == 4) return result;
        std::lock_guard lock(s.resolverMutex);
        if (!IsCore(module, result) || (s.core && s.core != module)) { Fail("NGX resolver changed module identity"); return result; }
        if (!s.core && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), &s.core)) { Fail("cannot retain the NGX core"); return result; }
        bool bound{}; FARPROC replacement{};
        switch (index) {
        case 0: bound=Bind(s.requirements,result); replacement=reinterpret_cast<FARPROC>(GetRequirements); break;
        case 1: bound=Bind(s.parameters[0],result); replacement=reinterpret_cast<FARPROC>(GetParameters<0>); break;
        case 2: bound=Bind(s.parameters[1],result); replacement=reinterpret_cast<FARPROC>(GetParameters<1>); break;
        case 3: bound=Bind(s.create,result); replacement=reinterpret_cast<FARPROC>(CreateFeature); break;
        }
        if (!bound) { Fail("NGX resolver changed an already bound function"); return result; }
        s.mask.fetch_or(1u << index); return replacement;
    } catch (...) { Fail("Ampere resolver preparation failed"); return result; }
}
bool Load(const std::filesystem::path& directory, const wchar_t* name, HMODULE& owned, bool allowSeparateModules) {
    const auto result = LoadConfiguredModule(directory / name, allowSeparateModules);
    owned = result.module;
    Message(ModuleLoadDiagnostic(result).c_str());
    return result.Succeeded() || Fail(result.Error());
}
bool BindAdapter(ID3D12Device* device) {
    auto& s=State(); s.luid=device->GetAdapterLuid();
    s.nvapi=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s.nvapi) return Fail("NVAPI could not be loaded");
    s.query=reinterpret_cast<Query>(GetProcAddress(s.nvapi,"nvapi_QueryInterface"));
    if (!s.query) return Fail("NVAPI query interface is missing");
    const auto init=reinterpret_cast<Initialize>(s.query(0x0150e828));
    const auto enumerate=reinterpret_cast<EnumGPUs>(s.query(0xe5ac921f));
    const auto getLuid=reinterpret_cast<GetLuid>(s.query(0x0ff07fde));
    s.arch=reinterpret_cast<GetArch>(s.query(0xd8265d24));
    if (!init || !enumerate || !getLuid || !s.arch || init()!=0) return Fail("NVAPI adapter functions are unavailable");
    std::array<void*,64> gpus{}; std::uint32_t count{};
    if (enumerate(gpus.data(),&count)!=0 || count==0 || count>gpus.size()) return Fail("NVAPI GPU enumeration failed");
    for (std::uint32_t i=0;i<count;++i) {
        LUID luid{};
        if (getLuid(gpus[i],&luid)!=0 || std::memcmp(&luid,&s.luid,sizeof(luid))) continue;
        if (s.gpu) return Fail("rendering adapter has ambiguous NVAPI identity");
        s.gpu=gpus[i];
    }
    ArchInfo arch{0x20010,0,0,0};
    if (!s.gpu || s.arch(s.gpu,&arch)!=0 || arch.architecture!=s.nativeArchitecture) return Fail("NVAPI does not match the physical compatibility adapter");
    if (s.targetSm==75) {
        const auto getName=reinterpret_cast<GetName>(s.query(0xceee8e9f));
        std::array<char,64> name{};
        if (!getName || getName(s.gpu,name.data())!=0 || name.back()!=0 || !IsRTX20(name.data()))
            return Fail("SM75 test requires a physical GeForce RTX 2060, 2070 or 2080; GTX 16 and other Turing products are not qualified");
        Message(name.data());
    }
    return true;
}
bool PlanProvider() {
    auto& s=State(); memory::Image image;
    if (!image.Open(s.provider) || !dlssg_provider_policy::IsDlssgImplementationModule(s.provider)) return Fail("Ampere provider ABI is unsupported");
    s.minimumArch=reinterpret_cast<std::uint8_t*>(GetProcAddress(s.provider,"NVSDK_NGX_GetGPUArchitecture"));
    const auto populate=reinterpret_cast<void*>(GetProcAddress(s.provider,"NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl"));
    constexpr std::array<std::uint8_t,6> gate{0xb8,0x90,1,0,0,0xc3};
    if (!image.OwnCode(s.minimumArch,gate.size()) || !image.OwnCode(populate) || !memory::Equal(s.minimumArch,gate)) return Fail("Ampere provider minimum-architecture instruction is unsupported");
    if (!midpoint_fix::BuildAmpereTemporalClone(s.provider,s.temporal,s.targetSm)) return Fail("Ampere temporal clone could not be built from the original program");
    std::size_t total{};
    for (const auto& section:image.sections) {
        if (!(section.Characteristics&IMAGE_SCN_MEM_READ) || (section.Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
        auto* begin=image.base+section.VirtualAddress; const auto size=section.Misc.VirtualSize;
        for (std::size_t p=0;p+16<=size;) {
            std::array<std::uint8_t,16> header{};
            if (!memory::Copy(header.data(),begin+p,header.size())) return Fail("Ampere provider data is unreadable");
            if (fatbin::ReadU32(header.data())!=fatbin::kMagic) { ++p;continue; }
            const auto payload=fatbin::ReadU64(header.data()+8);
            if (payload>size-p-16 || payload>fatbin::kMaxFatbinBytes-16) return Fail("Ampere provider fatbin bounds changed");
            const auto bytes=16+static_cast<std::size_t>(payload);
            std::vector<std::uint8_t> original(bytes);
            if (!memory::Copy(original.data(),begin+p,bytes)) return Fail("Ampere provider fatbin is unreadable");
            Plan plan; std::string reason; const auto result=RetargetForTarget(original.data(),original.size(),plan,reason,s.targetSm);
            if (result==trp::ampere::Status::Rejected) { Message(reason.c_str()); return Fail("Ampere provider program cannot be retargeted"); }
            if (result==trp::ampere::Status::Retargeted) {
                s.programs.push_back({begin+p,bytes});
                if (++s.fatbins>512 || bytes>64*1024*1024-total) return Fail("Ampere provider preparation exceeds its bounds");
                total+=bytes;
                if (plan.replacement.size()!=bytes) return Fail("provider replacement changed container allocation");
                // SM75 rebuilds whole compressed programs. Region-bounded chunks
                // keep startup writes practical and retain the same rollback owner.
                // SM86 keeps its existing independent-literal writes unchanged.
                if (s.targetSm==75) {
                    for (std::size_t j=0;j<bytes;) {
                        MEMORY_BASIC_INFORMATION region{};
                        if (!VirtualQuery(begin+p+j,&region,sizeof(region)) || region.State!=MEM_COMMIT ||
                            (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return Fail("Turing provider region unreadable");
                        const auto available=region.RegionSize-(reinterpret_cast<std::uintptr_t>(begin+p+j)-reinterpret_cast<std::uintptr_t>(region.BaseAddress));
                        const auto count=(std::min)({bytes-j,available,std::size_t{4096}});
                        if (!count) return Fail("Turing provider region empty");
                        if (!std::equal(original.begin()+j,original.begin()+j+count,plan.replacement.begin()+j))
                            s.data.Add(begin+p+j,{original.data()+j,count},{plan.replacement.data()+j,count});
                        j+=count;
                    }
                } else {
                    for (std::size_t j=0;j<bytes;++j) if (original[j]!=plan.replacement[j]) s.data.Add(begin+p+j,{original.data()+j,1},{plan.replacement.data()+j,1});
                }
            }
            p+=bytes;
        }
    }
    if (!s.fatbins) return Fail("Ampere provider has no retargetable programs");
    using namespace TheosRenderPipeline::SourceDLSSG;
    // Two independent MFG architecture decisions, qualified by surrounding
    // instructions. Unlike a bare immediate scan, unrelated 0x1b0 data is ignored.
    constexpr std::array<std::uint8_t,8> first{0x81,0xfd,0xb0,1,0,0,0x0f,0x8c};
    constexpr std::array<std::uint8_t,5> countFive{0xbf,5,0,0,0};
    constexpr std::array<std::uint8_t,11> second{0x3d,0xb0,1,0,0,0x0f,0x93,0xc0,0x88,0x47,0x28};
    const auto firstSite=MFGPatch::FindExecutable(s.provider,17,[&](auto b){
        return std::equal(first.begin(),first.end(),b.begin()) && std::equal(countFive.begin(),countFive.end(),b.begin()+12);
    });
    const auto secondSite=MFGPatch::FindExecutable(s.provider,second.size(),[&](auto b){return std::equal(second.begin(),second.end(),b.begin());});
    if (!firstSite || !secondSite) return Fail("Ampere MFG architecture instruction contracts changed");
    DWORD64 base{};
    const auto* function=RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(firstSite),&base,nullptr);
    std::int32_t displacement{};
    if (!function || base!=reinterpret_cast<DWORD64>(s.provider) ||
        !memory::Read(firstSite+8,displacement)) return Fail("Ampere MFG branch function is unsupported");
    const auto rva=reinterpret_cast<DWORD64>(firstSite)-base;
    const auto destination=static_cast<std::int64_t>(rva)+12+displacement;
    if (rva<function->BeginAddress || rva+17>function->EndAddress ||
        destination<function->BeginAddress || destination>=function->EndAddress) return Fail("Ampere MFG branch leaves its function");
    const std::uint8_t before=0xb0,after=static_cast<std::uint8_t>(s.nativeArchitecture);
    s.data.Add(firstSite+2,{&before,1},{&after,1}); s.data.Add(secondSite+1,{&before,1},{&after,1});
    const auto clamp=MFGPatch::FindExecutable(s.wrapper,MFGContract::wrapperPattern.size(),MFGContract::MatchesWrapper);
    if (!clamp) return Fail("Ampere wrapper capacity instruction contract changed");
    constexpr std::array<std::uint8_t,3> nops{0x90,0x90,0x90};
    s.data.Add(clamp+7,std::span(MFGContract::wrapperPattern).subspan(7,3),nops);
    // Publish the fully composed temporal clone before admitting the provider.
    s.data.Add(reinterpret_cast<void*>(s.temporal.slot),
        {reinterpret_cast<std::uint8_t*>(&s.temporal.originalDescriptor),sizeof(void*)},
        {reinterpret_cast<std::uint8_t*>(&s.temporal.replacementDescriptor),sizeof(void*)});
    const std::uint8_t minimumBefore=0x90;
    s.data.Add(s.minimumArch+1,{&minimumBefore,1},{&after,1});
    std::uintptr_t temporalBlob{};
    if(!memory::Read(reinterpret_cast<const void*>(s.temporal.replacementDescriptor+8),temporalBlob))return Fail("prepared temporal descriptor is unreadable");
    s.programs.push_back({reinterpret_cast<const void*>(temporalBlob),s.temporal.outputBytes});
    return true;
}
} // namespace

bool Start(ID3D12Device* device,const std::filesystem::path& directory,Log log,bool allowSeparateModules,Fatal fatal) noexcept {
    auto& s=State();
    if (s.started.exchange(true)) return Fail("Ampere startup cannot be repeated");
    s.log=log;
    try {
        Message("stage=adapter-binding");
        const auto adapter=device ? midpoint_fix::ObserveD3D12Adapter(device) : midpoint_fix::AdapterKind::Unavailable;
        if (!device || !directory.is_absolute() || (adapter!=midpoint_fix::AdapterKind::Ampere && adapter!=midpoint_fix::AdapterKind::Turing))
            return Fail("Compatibility preparation requires an actual SM86 or SM75 rendering adapter");
        if (adapter==midpoint_fix::AdapterKind::Turing) {
            s.targetSm=75; s.nativeArchitecture=kTuring;s.fatal=fatal;
            if(!s.fatal)return Fail("Turing kernel failure guard requires a terminating host callback");
        }
        Message(s.targetSm==75 ? "physical adapter=Turing target=SM75; experimental instruction lowering" : "physical adapter=Ampere target=SM86");
        if (!BindAdapter(device)) return false;
        if (!Load(directory,L"nvngx_dlssg.dll",s.provider,allowSeparateModules)) return false;
        if (!Load(directory,L"sl.common.dll",s.common,allowSeparateModules)) return false;
        if (!Load(directory,L"sl.dlss_g.dll",s.wrapper,allowSeparateModules)) return false;
        Message("stage=provider-plan");
        if (!PlanProvider()) return false;
        Message("stage=resolver-plan");
        const std::array<HMODULE,3> modules{s.common,s.wrapper,s.provider};
        const std::array<Resolver,3> replacements{Resolve<0>,Resolve<1>,Resolve<2>};
        for (std::size_t i=0;i<(s.targetSm==75?3u:2u);++i) {
            memory::Image image;
            if (!image.Open(modules[i]) || !(s.importSlots[i]=image.Import("GetProcAddress")) ||
                !memory::Read(s.importSlots[i],s.resolvers[i]) || !s.resolvers[i]) return Fail("Ampere resolver import is missing or ambiguous");
            s.imports.Add(s.importSlots[i],{reinterpret_cast<std::uint8_t*>(&s.resolvers[i]),sizeof(void*)},
                {reinterpret_cast<const std::uint8_t*>(&replacements[i]),sizeof(void*)});
        }
        Message("stage=provider-publication");
        if (!s.data.Commit()) return Fail(s.data.Unsafe() ? "Ampere provider rollback failed; restart required" : "Ampere provider preparation rolled back");
        s.prepared.store(true);
        Message("stage=resolver-publication");
        if (!s.imports.Commit()) return Fail(s.imports.Unsafe() ? "Ampere resolver rollback failed; restart required" : "Ampere resolver installation failed; restart required");
        s.installed.store(true);
        Message(s.targetSm==75 ? "Turing provider and temporal program prepared before Streamline initialization" : "Ampere provider and temporal program prepared before Streamline initialization");
        return true;
    } catch (...) { return Fail("Ampere startup preparation raised an exception; restart required"); }
}
RuntimeStatus Snapshot() noexcept {
    auto& s=State(); return {s.prepared.load(),s.installed.load(),s.failed.load(),s.createSeen.load(),s.fatbins,s.mask.load(),s.requirementsCalls.load(),s.capabilityCalls.load(),s.createCalls.load(),s.error.load()};
}
bool Verify() noexcept {
    auto& s=State(); if (!Prepared()) return false;
    std::uintptr_t descriptor{};
    if (!memory::Read(reinterpret_cast<void*>(s.temporal.slot),descriptor) || descriptor!=s.temporal.replacementDescriptor ||
        !s.minimumArch || s.minimumArch[1]!=static_cast<std::uint8_t>(s.nativeArchitecture)) return Fail("Ampere provider publication changed; restart required");
    const std::array<Resolver,3> expected{Resolve<0>,Resolve<1>,Resolve<2>};
    for (std::size_t i=0;i<(s.targetSm==75?3u:2u);++i) {
        Resolver observed{};
        if (!memory::Read(s.importSlots[i],observed) || observed!=expected[i]) return Fail("Ampere resolver publication changed; restart required");
    }
    return true;
}
void EnterStartupScope() noexcept { ++startupScope; }
void LeaveStartupScope() noexcept { if (startupScope) --startupScope; }
} // namespace trp::ampere
