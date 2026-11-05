#include "FrameGen/XeFGTensorAPI.h"
#include "FrameGen/XeFGPacingAPI.h"
#include <cstring>
using namespace TheosRenderPipeline::XeFGTensor;
namespace {
unsigned starts{}, retires{}, flags{};
bool Mode(const char* mode){char text[64]{};GetEnvironmentVariableA("TRP_XEFG_MOCK_MODE",text,sizeof(text));return !std::strcmp(text,mode);}
HRESULT __cdecl Start(ID3D12Device*,HMODULE,HMODULE,const wchar_t*,char*,std::uint32_t){
    ++starts;
    if(Mode("safe-start")){flags=0;return E_FAIL;}
    if(Mode("unsafe-start")){flags=Unsafe;return E_FAIL;}
    flags=ModelReady;return S_OK;
}
HRESULT __cdecl Retire(char*,std::uint32_t){++retires;return Mode("pacing-success")?S_OK:E_FAIL;} // Deliberately does not set Unsafe.
void __cdecl Snapshot(Stats* s){if(s&&s->size==sizeof(Stats)){s->flags=flags;s->matchedPSOs=starts;s->cachedPSOs=retires;}}
}
namespace {
unsigned pacingFlags{};
HRESULT __cdecl RegisterPacing(void*,ID3D12Device*,char*,std::uint32_t){
    if(Mode("pacing-safe-register"))return S_FALSE;
    pacingFlags|=TheosRenderPipeline::XeFGPacing::Registered;
    return Mode("pacing-unsafe-register")?E_FAIL:S_OK;
}
HRESULT __cdecl UnregisterPacing(void*,char*,std::uint32_t){
    if(Mode("pacing-retire-fail"))return E_FAIL;
    pacingFlags&=TheosRenderPipeline::XeFGPacing::VSyncCadenceRequested;return S_OK;
}
void __cdecl ResetPacing(void*){}
void __cdecl SetMetering(bool){}
void __cdecl SetVSyncCadence(bool enabled){if(enabled)pacingFlags|=TheosRenderPipeline::XeFGPacing::VSyncCadenceRequested;else pacingFlags&=~TheosRenderPipeline::XeFGPacing::VSyncCadenceRequested;}
void __cdecl PacingSnapshot(TheosRenderPipeline::XeFGPacing::Stats* s){if(s&&s->size==sizeof(*s))s->flags=pacingFlags;}
void __cdecl SetLogger(void (*)(const char*)){}
}
extern "C" __declspec(dllexport) HRESULT __cdecl TRPXeFGPacingGetAPI(TheosRenderPipeline::XeFGPacing::API* api){
    using namespace TheosRenderPipeline::XeFGPacing;
    if(Mode("pacing-api-version")||!api||api->size!=sizeof(*api)||api->version!=TheosRenderPipeline::XeFGPacing::Version)return E_NOINTERFACE;
    api->registerContext=&RegisterPacing;api->unregisterContext=&UnregisterPacing;api->reset=&ResetPacing;
    api->setVSyncCadence=&SetVSyncCadence;api->setMetering=&SetMetering;api->snapshot=&PacingSnapshot;api->setLogger=&SetLogger;return S_OK;
}
extern "C" __declspec(dllexport) HRESULT __cdecl TRPXeFGTensorGetAPI(API* api){
    if(!api||api->size!=sizeof(API)||api->version!=Version)return E_NOINTERFACE;
    api->start=&Start;api->retire=&Retire;api->snapshot=&Snapshot;return S_OK;
}
