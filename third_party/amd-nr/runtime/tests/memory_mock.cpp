// CPU oracle for gate ABI/forwarding and probe readback; never a GPU test.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

static void* context{};
static int selected = -1;
static bool flag(const char* key) { char text[8]{}; return GetEnvironmentVariableA(key, text, 8) && text[0] == '1'; }
static void record(const char* name) {
    char path[32768]{};
    if (GetEnvironmentVariableA("MOCK_CALL_LOG", path, sizeof(path))) {
        FILE* f = std::fopen(path, "a"); if (f) { std::fprintf(f, "%s\n", name); std::fclose(f); }
    }
}
extern "C" {
__declspec(dllexport) int cuInit(unsigned) { return 0; }
__declspec(dllexport) int cuDeviceGetCount(int* n) { *n = 2; return 0; }
__declspec(dllexport) int cuDeviceGet(int* d, int ordinal) { *d = ordinal; return 0; }
__declspec(dllexport) int cuDeviceGetName(char* out, int size, int d) { std::snprintf(out, size, "AMD CPU MOCK %s [ZLUDA]", d == 1 ? "7900 XTX" : "integrated"); return 0; }
__declspec(dllexport) int cuDeviceGetPCIBusId(char* out, int size, int d) { std::snprintf(out, size, "%s", d == 1 ? "0000:03:00.0" : "0000:0e:00.0"); return 0; }
__declspec(dllexport) int cuDeviceGetLuid(char* id, unsigned* mask, int d) { unsigned long long value = d == 1 ? 0x13d45 : 0x16bff; std::memcpy(id, &value, 8); *mask = 1u << d; return 0; }
__declspec(dllexport) int cuCtxCreate_v2(void** out, unsigned, int d) { selected = d; context = reinterpret_cast<void*>(0x123400ull + d); *out = context; return 0; }
__declspec(dllexport) int cuCtxDestroy_v2(void*) { context = nullptr; selected = -1; return 0; }
__declspec(dllexport) int cuCtxGetCurrent(void** out) { *out = context; return 0; }
__declspec(dllexport) int cuCtxGetDevice(int* out) { *out = selected; return context ? 0 : 201; }
__declspec(dllexport) int cuCtxSynchronize() { record("cuCtxSynchronize"); return 0; }
__declspec(dllexport) int cuStreamCreate(void** out, unsigned) { *out = reinterpret_cast<void*>(0x567800); return 0; }
__declspec(dllexport) int cuStreamDestroy_v2(void*) { return 0; }
__declspec(dllexport) int cuStreamSynchronize(void*) { record("cuStreamSynchronize"); return 0; }
__declspec(dllexport) int cuMemcpyDtoH_v2(void* out, unsigned long long ptr, size_t bytes) { std::memcpy(out, reinterpret_cast<void*>(ptr), bytes); return 0; }
__declspec(dllexport) int cuMemAlloc_v2(unsigned long long* out, size_t bytes) {
    record("cuMemAlloc_v2"); if (flag("MOCK_FAIL_ALLOC")) return 2;
    *out = reinterpret_cast<unsigned long long>(std::malloc(bytes)); return *out ? 0 : 2;
}
__declspec(dllexport) int cuMemFree_v2(unsigned long long ptr) { record("cuMemFree_v2"); std::free(reinterpret_cast<void*>(ptr)); return 0; }
// Legacy ABI deliberately reports unsupported and must remain a forwarder.
__declspec(dllexport) int cuMemAlloc(unsigned*, unsigned) { record("cuMemAlloc-legacy"); return 801; }
__declspec(dllexport) int cuMemFree(unsigned) { return 801; }
__declspec(dllexport) int cuMemAllocManaged(unsigned long long* p, size_t n, unsigned) { return cuMemAlloc_v2(p, n); }
__declspec(dllexport) int cuMemAllocPitch_v2(unsigned long long* p, size_t* pitch, size_t w, size_t h, unsigned) { *pitch = w; return cuMemAlloc_v2(p, w*h); }
#define ALLOC_ASYNC(Suffix) \
__declspec(dllexport) int cuMemAllocAsync##Suffix(unsigned long long* p, size_t n, void*) { record("cuMemAllocAsync" #Suffix); return cuMemAlloc_v2(p,n); } \
__declspec(dllexport) int cuMemAllocFromPoolAsync##Suffix(unsigned long long* p, size_t n, void*, void*) { record("cuMemAllocFromPoolAsync" #Suffix); return cuMemAlloc_v2(p,n); } \
__declspec(dllexport) int cuMemFreeAsync##Suffix(unsigned long long p, void*) { record("cuMemFreeAsync" #Suffix); return cuMemFree_v2(p); }
__declspec(dllexport) int cuMemAllocAsync(unsigned long long* p, size_t n, void*) { record("cuMemAllocAsync"); return cuMemAlloc_v2(p,n); }
__declspec(dllexport) int cuMemAllocFromPoolAsync(unsigned long long* p, size_t n, void*, void*) { record("cuMemAllocFromPoolAsync"); return cuMemAlloc_v2(p,n); }
__declspec(dllexport) int cuMemFreeAsync(unsigned long long p, void*) { record("cuMemFreeAsync"); return cuMemFree_v2(p); }
ALLOC_ASYNC(_ptsz)
}
template<class Value> int fill(unsigned long long ptr, size_t pitch, Value value, size_t w, size_t h, const char* name) {
    record(name);
    if (flag("MOCK_FAIL_FILL")) return 700;
    if (flag("MOCK_VARIANT_RESULTS") && std::strstr(name, "_pt")) return 77;
    for (size_t y = 0; y < h; ++y) {
        auto target = reinterpret_cast<Value*>(ptr + pitch*y);
        for (size_t x = 0; x < w; ++x) target[x] = value;
    }
    return 0;
}
extern "C" {
#define FILL(Bits, Type) \
__declspec(dllexport) int cuMemsetD##Bits##_v2(unsigned long long p, Type v, size_t n) { return fill(p,0,v,n,1,"cuMemsetD" #Bits "_v2"); } \
__declspec(dllexport) int cuMemsetD##Bits##_v2_ptds(unsigned long long p, Type v, size_t n) { return fill(p,0,v,n,1,"cuMemsetD" #Bits "_v2_ptds"); } \
__declspec(dllexport) int cuMemsetD##Bits##Async(unsigned long long p, Type v, size_t n, void*) { return fill(p,0,v,n,1,"cuMemsetD" #Bits "Async"); } \
__declspec(dllexport) int cuMemsetD##Bits##Async_ptsz(unsigned long long p, Type v, size_t n, void*) { return fill(p,0,v,n,1,"cuMemsetD" #Bits "Async_ptsz"); } \
__declspec(dllexport) int cuMemsetD2D##Bits##_v2(unsigned long long p, size_t pitch, Type v, size_t w, size_t h) { return fill(p,pitch,v,w,h,"cuMemsetD2D" #Bits "_v2"); } \
__declspec(dllexport) int cuMemsetD2D##Bits##_v2_ptds(unsigned long long p, size_t pitch, Type v, size_t w, size_t h) { return fill(p,pitch,v,w,h,"cuMemsetD2D" #Bits "_v2_ptds"); } \
__declspec(dllexport) int cuMemsetD2D##Bits##Async(unsigned long long p, size_t pitch, Type v, size_t w, size_t h, void*) { return fill(p,pitch,v,w,h,"cuMemsetD2D" #Bits "Async"); } \
__declspec(dllexport) int cuMemsetD2D##Bits##Async_ptsz(unsigned long long p, size_t pitch, Type v, size_t w, size_t h, void*) { return fill(p,pitch,v,w,h,"cuMemsetD2D" #Bits "Async_ptsz"); }
FILL(8, unsigned char)
FILL(16, unsigned short)
FILL(32, unsigned)
__declspec(dllexport) int cuModuleGetFunction(void**, void*, const char*) { return 801; }
__declspec(dllexport) int cuLaunchKernel(void*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, void*, void**, void**) { return 801; }
__declspec(dllexport) int cuGetProcAddress(const char* symbol, void** out, int version, unsigned long long flags) {
    if (!symbol || !out) return 1;
    if (std::strcmp(symbol, "unknown") == 0) { *out = nullptr; return 500; }
    std::string name(symbol);
    if (version >= 3020 && (name == "cuMemAlloc" || name == "cuMemFree" ||
        (name.find("cuMemset") == 0 && name.find("Async") == std::string::npos && name.find("_v2") == std::string::npos))) name += "_v2";
    if (flags == 2 && name.find("cuMemset") == 0) name += name.find("Async") == std::string::npos ? "_ptds" : "_ptsz";
    auto module = GetModuleHandleW(L"zluda_real.dll");
    *out = reinterpret_cast<void*>(GetProcAddress(module, name.c_str()));
    return *out ? 0 : 500;
}
__declspec(dllexport) int cuGetProcAddress_v2(const char* symbol, void** out, int version, unsigned long long flags, int* status) {
    const int r = cuGetProcAddress(symbol, out, version, flags); if (status) *status = r == 0 ? 0 : 1; return r;
}
}
