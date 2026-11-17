// Private TRP addition, under the root project LICENSE. Upstream NVAPI remains
// an unchanged separate library; its other QueryInterface responses are forwarded.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "nvapi_adapter.h"
#include <cstring>
#include <mutex>
#include <string>

namespace {
std::once_flag loadOnce;
TRPNvapi::Query upstream{};
using LogicalInfo=int (*)(void*, TRPNvapi::LogicalData*);
LogicalInfo originalLogical{};
std::mutex selectionMutex;
TRPNvapi::GetLuid getLuid{};
int selectedDevice{-1};
std::uint64_t selectedLuid{};

void LoadUpstream() {
    HMODULE self{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadUpstream), &self)) return;
    wchar_t path[32768]{};
    const auto length=GetModuleFileNameW(self,path,32768);
    if (!length || length>=32768) return;
    std::wstring sibling(path,length);
    const auto slash=sibling.find_last_of(L"\\/");
    if(slash==std::wstring::npos) return;
    sibling.resize(slash+1); sibling+=L"nvapi_trp_upstream.dll";
    // Full path, retained for process lifetime like the other model modules.
    auto library=LoadLibraryExW(sibling.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!library) return;
    wchar_t loaded[32768]{};
    if(!GetModuleFileNameW(library,loaded,32768) || _wcsicmp(loaded,sibling.c_str())!=0) return;
    upstream=reinterpret_cast<TRPNvapi::Query>(GetProcAddress(library,"nvapi_QueryInterface"));
    if(upstream) originalLogical=reinterpret_cast<LogicalInfo>(upstream(TRPNvapi::LogicalInfoId));
}
int Logical(void* gpu, TRPNvapi::LogicalData* data) {
    if(!data || data->version!=TRPNvapi::LogicalVersion) return -9;
    std::lock_guard<std::mutex> guard(selectionMutex);
    if(!getLuid || selectedDevice<0 || !selectedLuid || !originalLogical) return -1;
    char bytes[8]{}; unsigned mask{};
    std::uint64_t actual{};
    if(getLuid(bytes,&mask,selectedDevice)!=0) return -1;
    std::memcpy(&actual,bytes,8);
    if(actual!=selectedLuid) return -1;
    // Preserve the upstream physical handle/topology convention, but supply
    // the independently validated selected adapter instead of CUDA ordinal 0.
    const auto status=originalLogical(gpu,data);
    if(status==0 && data->osAdapterId) std::memcpy(data->osAdapterId,bytes,8);
    return status;
}
}
extern "C" __declspec(dllexport) void* nvapi_QueryInterface(std::uint32_t id) {
    std::call_once(loadOnce,LoadUpstream);
    if(!upstream) return nullptr;
    if(id==TRPNvapi::LogicalInfoId) return originalLogical ? reinterpret_cast<void*>(&Logical) : nullptr;
    return upstream(id);
}
extern "C" __declspec(dllexport) std::uint32_t dlss5nr_trp_nvapi_select_adapter(
    std::uint64_t expectedLuid, int device, TRPNvapi::GetLuid query) {
    std::call_once(loadOnce,LoadUpstream);
    if(!upstream || !originalLogical || !expectedLuid || device<0 || !query) return 0;
    char bytes[8]{};unsigned mask{};std::uint64_t actual{};
    if(query(bytes,&mask,device)!=0) return 0;
    std::memcpy(&actual,bytes,8);
    if(actual!=expectedLuid) return 0;
    std::lock_guard<std::mutex> guard(selectionMutex);
    // A retained model chain must not silently migrate between physical GPUs.
    if(selectedLuid && selectedLuid!=expectedLuid) return 0;
    selectedLuid=actual;selectedDevice=device;getLuid=query;
    return 1;
}
