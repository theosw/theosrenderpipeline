#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../third_party/amd-nr/nvapi_adapter.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
void Require(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
int main() {
    try {
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
        auto directory=std::filesystem::path(exe).parent_path();
        SetEnvironmentVariableW(L"ZLUDA_NVAPI_GPU_ARCH",L"0x1B0");
        auto driver=LoadLibraryW((directory/L"nvcuda.dll").c_str());Require(driver,"fake driver load");
        auto original=LoadLibraryW((directory/L"nvapi_trp_upstream.dll").c_str());Require(original,"exact upstream shim load");
        auto adapter=LoadLibraryW((directory/L"nvapi64.dll").c_str());Require(adapter,"adapter load");
        auto query=reinterpret_cast<TRPNvapi::Query>(GetProcAddress(original,"nvapi_QueryInterface"));
        auto wrapped=reinterpret_cast<TRPNvapi::Query>(GetProcAddress(adapter,"nvapi_QueryInterface"));
        auto select=reinterpret_cast<std::uint32_t (*)(std::uint64_t,int,TRPNvapi::GetLuid)>(GetProcAddress(adapter,"dlss5nr_trp_nvapi_select_adapter"));
        auto luidQuery=reinterpret_cast<TRPNvapi::GetLuid>(GetProcAddress(driver,"cuDeviceGetLuid"));
        Require(query && wrapped && select && luidQuery,"exports");
        using GetLogical=int (*)(void*,TRPNvapi::LogicalData*);
        auto logical=reinterpret_cast<GetLogical>(query(TRPNvapi::LogicalInfoId));
        auto corrected=reinterpret_cast<GetLogical>(wrapped(TRPNvapi::LogicalInfoId));
        std::uint64_t luid{};
        TRPNvapi::LogicalData data{};data.version=TRPNvapi::LogicalVersion;data.osAdapterId=&luid;
        Require(logical && corrected && logical(reinterpret_cast<void*>(1),&data)==0 && luid==0x19981,"negative control: upstream still reports iGPU ordinal 0");
        Require(corrected(reinterpret_cast<void*>(1),&data)!=0,"unconfigured correction fails closed");
        Require(!select(0x17573,0,luidQuery),"wrong physical GPU rejected");
        Require(select(0x17573,1,luidQuery)==1,"select actual discrete GPU");
        Require(corrected(reinterpret_cast<void*>(1),&data)==0 && luid==0x17573,"logical LUID follows discrete GPU");
        Require(data.physicalGpuCount==1 && data.physicalGpuHandles[0]==reinterpret_cast<void*>(1),"original topology preserved");
        Require(select(0x17573,1,luidQuery)==1 && !select(0x19981,0,luidQuery),"same GPU recreation allowed, retained chain migration rejected");
        data.version=0;Require(corrected(reinterpret_cast<void*>(1),&data)!=0,"unsupported structure version rejected");
        Require(wrapped(TRPNvapi::ArchInfoId)==query(TRPNvapi::ArchInfoId),"architecture implementation forwarded unchanged");
        using GetArch=int (*)(void*,TRPNvapi::ArchInfo*);
        TRPNvapi::ArchInfo arch{TRPNvapi::ArchVersion};
        Require(reinterpret_cast<GetArch>(wrapped(TRPNvapi::ArchInfoId))(reinterpret_cast<void*>(1),&arch)==0 && arch.architecture==0x1b0,"original architecture override works");
        Require(wrapped(0xdeadbeef)==query(0xdeadbeef),"unknown query preserved");
        std::printf("PASS exact upstream negative control: LUID 19981 -> corrected 17573, architecture %x; no GPU/context created\n",arch.architecture);
        return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
