#include <cstdint>
#include <cstring>
extern "C" __declspec(dllexport) int cuDeviceGet(int* device,int ordinal) {
    if(!device || ordinal<0 || ordinal>1) return 1;
    *device=ordinal;return 0;
}
extern "C" __declspec(dllexport) int cuDeviceGetLuid(char* bytes,unsigned* mask,int device) {
    if(!bytes || !mask || device<0 || device>1) return 1;
    const std::uint64_t luid=device==0 ? 0x19981 : 0x17573;
    std::memcpy(bytes,&luid,8);*mask=1u<<device;return 0;
}
