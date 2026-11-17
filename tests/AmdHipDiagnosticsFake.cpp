#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
namespace { int device=1; bool failed=false; }
extern "C" {
__declspec(dllexport) void trp_test_select(int d, int failure) { device=d; failed=failure!=0; }
__declspec(dllexport) int hipCtxGetCurrent(void** out) { if(failed) return 201; *out=reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000+device)); return 0; }
__declspec(dllexport) int hipGetDevice(int* out) { if(failed) return 202; *out=device; return 0; }
__declspec(dllexport) int hipDeviceGetLuid(char* out,unsigned* mask,int d) { uint64_t id=d ? 0x17573 : 0x19981; std::memcpy(out,&id,8); *mask=1u<<d; return 0; }
__declspec(dllexport) int hipDeviceGetPCIBusId(char* out,int size,int d) { std::snprintf(out,size,"0000:%02x:00.0",d ? 3 : 9); return 0; }
}
