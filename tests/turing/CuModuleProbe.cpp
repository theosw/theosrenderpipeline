// Manual driver-loading probe. Creates a device and modules, never dispatches
// kernels or initializes NGX/Streamline. Local results are not RTX20 acceptance.
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
using Microsoft::WRL::ComPtr;
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 3;
    ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
    for(UINT i=0;;++i) {
        adapter.Reset();if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)return 4;
        if(SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))break;
    }
    ComPtr<ID3D12Device> device;
    if(FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))return 5;
    std::wcout<<L"device="<<desc.Description<<L" no GPU dispatch\n";
    const auto library=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Query=void* (__cdecl*)(unsigned);
    const auto query=reinterpret_cast<Query>(GetProcAddress(library,"nvapi_QueryInterface"));if(!query)return 6;
    const auto init=reinterpret_cast<int (__cdecl*)()>(query(0x0150e828));if(!init || init())return 7;
    using Create=int (__cdecl*)(ID3D12Device*,const void*,unsigned,void**);
    using Destroy=int (__cdecl*)(ID3D12Device*,void*);
    const auto create=reinterpret_cast<Create>(query(0xad1a677d));
    const auto destroy=reinterpret_cast<Destroy>(query(0x41c65285));if(!create || !destroy)return 8;
    unsigned total{},failed{};
    for(const auto& entry:std::filesystem::directory_iterator(argv[1])) {
        if(entry.path().extension()!=L".fatbin")continue;
        std::ifstream file(entry.path(),std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});if(bytes.empty())return 9;
        void* module{};
        const int status=create(device.Get(),bytes.data(),static_cast<unsigned>(bytes.size()),&module);
        ++total;if(status || !module)++failed;
        std::wcout<<entry.path().filename().wstring()<<L" bytes="<<bytes.size()<<L" status="<<status<<L" handle="<<(module!=nullptr)<<std::endl;
        if(module && destroy(device.Get(),module))return 10;
    }
    std::wcout<<L"total="<<total<<L" failed="<<failed<<std::endl;
    return total==71 && !failed?0:1;
}
