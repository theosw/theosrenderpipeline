#include "PacingBridge.h"
#include "pacing/PacingSupport.h"
#include "pacing/sdk_pacing.h"
#include <atomic>
#include <mutex>
#include <cstring>
namespace {
using namespace TheosRenderPipeline::XeFGPacing;
std::mutex mutex;
std::atomic<void (*)(const char*)> logger{};
sdk_pacing::SetFlipConfigFunction setFlip{};
bool installed{}, requested{true};
std::string failure;
void* context{};
uint64_t generation{};
void Reason(char* output,uint32_t bytes,const std::string& message) {
    if(output && bytes){const auto n=(std::min)(message.size(),size_t(bytes-1));std::memcpy(output,message.data(),n);output[n]=0;}
}
HRESULT __cdecl Register(void* handle,ID3D12Device* device,char* reason,uint32_t bytes) {
    try {
    std::lock_guard lock(mutex);
    if(sdk_pacing::Snapshot().flags&Unsafe){Reason(reason,bytes,"XeFG pacing owner unsafe; restart required");return E_UNEXPECTED;}
    if(!installed){Reason(reason,bytes,"SDK pacing retained; private pacing unavailable: "+failure);return S_FALSE;}
    if(context){Reason(reason,bytes,"Another XeFG pacing context is retained");return E_UNEXPECTED;}
    std::string why;
    const auto next=generation+1;
    if(!sdk_pacing::Register(handle,next,device,why)) {Reason(reason,bytes,"SDK pacing retained; registration refused: "+why);return S_FALSE;}
    context=handle;generation=next;
    Reason(reason,bytes,setFlip ? "XeFG v1.6 cadence registered; NVIDIA flip metering awaiting driver feedback" : "XeFG v1.6 cadence registered; native NVIDIA flip metering unavailable");
    return S_OK;
    }catch(...){if(reason&&bytes)reason[0]=0;return E_FAIL;}
}
HRESULT __cdecl Unregister(void* handle,char* reason,uint32_t bytes) {
    try {
    std::lock_guard lock(mutex);
    if(sdk_pacing::Snapshot().flags&Unsafe){Reason(reason,bytes,"XeFG pacing owner unsafe; retained");return E_UNEXPECTED;}
    if(!context){Reason(reason,bytes,"No XeFG pacing owner");return S_OK;}
    if(handle!=context){Reason(reason,bytes,"XeFG pacing retirement identity mismatch");return E_UNEXPECTED;}
    if(!sdk_pacing::Unregister(context,generation)) {Reason(reason,bytes,"XeFG driver metering retirement refused; owner retained");return E_FAIL;}
    context=nullptr;Reason(reason,bytes,"XeFG cadence retired; driver metering off; hooks forwarding");return S_OK;
    }catch(...){if(reason&&bytes)reason[0]=0;return E_FAIL;}
}
void __cdecl Reset(void* handle){try{std::lock_guard lock(mutex);if(context==handle)sdk_pacing::Reset(context,generation);}catch(...){}}
void __cdecl SetMetering(bool enabled){try{std::lock_guard lock(mutex);requested=enabled;sdk_pacing::SetFlipMetering(setFlip,enabled,2500000);}catch(...){}}
void __cdecl SetVSyncCadence(bool enabled){try{std::lock_guard lock(mutex);sdk_pacing::SetVSyncCadence(enabled);}catch(...){}}
void __cdecl Snapshot(Stats* output){if(output && output->size==sizeof(Stats))*output=sdk_pacing::Snapshot();}
void __cdecl SetLogger(void (*callback)(const char*)){logger.store(callback,std::memory_order_release);}
}
namespace trp_pacing_bridge {
HMODULE Self() noexcept {HMODULE module{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Self),&module);return module;}
void WriteLog(const std::string& event,const std::string& fields) {
    const auto message="[XeFG pacing] "+event+" "+fields;
    if(const auto callback=logger.load(std::memory_order_acquire))callback(message.c_str());
    else OutputDebugStringA((message+"\n").c_str());
}
}
bool TRPPacingHasOwner() noexcept {return (sdk_pacing::Snapshot().flags&TheosRenderPipeline::XeFGPacing::Registered)!=0;}
bool TRPPacingIsUnsafe() noexcept {return (sdk_pacing::Snapshot().flags&TheosRenderPipeline::XeFGPacing::Unsafe)!=0;}
void TRPPreparePacing(ID3D12Device*,HMODULE fg) noexcept {
    std::lock_guard lock(mutex);
    if(installed || !failure.empty())return;
    try {
#if defined(TRP_XEFG_EXTENSION_TESTING)
        char fault[16]{};GetEnvironmentVariableA("TRP_XEFG_PACING_FAULT",fault,sizeof(fault));
        if(fault[0]=='2'){failure="injected pacing installation refusal";return;}
#endif
        HMODULE nvapi=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(nvapi){
            HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(nvapi),&pinned);
            using Query=void*(__cdecl*)(uint32_t);using Initialize=int(__cdecl*)();
            const auto query=reinterpret_cast<Query>(GetProcAddress(nvapi,"nvapi_QueryInterface"));
            const auto initialize=query?reinterpret_cast<Initialize>(query(0x0150e828u)):nullptr;
            if(initialize && initialize()==0)setFlip=reinterpret_cast<sdk_pacing::SetFlipConfigFunction>(query(0xf3148c42u));
        }
        sdk_pacing::SetFlipMetering(setFlip,requested,2500000);
        installed=sdk_pacing::Install(fg,failure);
    }catch(const std::exception& error){failure=error.what();}
    catch(...){failure="pacing_prepare_exception";}
}
extern "C" __declspec(dllexport) HRESULT __cdecl TRPXeFGPacingGetAPI(TheosRenderPipeline::XeFGPacing::API* api){
    using namespace TheosRenderPipeline::XeFGPacing;
    if(!api || api->size!=sizeof(API) || api->version!=Version)return E_NOINTERFACE;
    api->registerContext=&Register;api->unregisterContext=&Unregister;api->reset=&Reset;
    api->setVSyncCadence=&SetVSyncCadence;api->setMetering=&SetMetering;api->snapshot=&Snapshot;api->setLogger=&SetLogger;return S_OK;
}
