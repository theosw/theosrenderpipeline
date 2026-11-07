#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while(0)
int main(int argc, char** argv) {
    CHECK(argc == 3);
    auto real = LoadLibraryA(argv[1]); CHECK(real);
    auto gate = LoadLibraryA(argv[2]); CHECK(gate);
    auto gpa = reinterpret_cast<int (*)(const char*, void**, int, unsigned long long, int*)>(GetProcAddress(gate, "cuGetProcAddress_v2")); CHECK(gpa);
    char enabled[8]{}; const bool tracing = GetEnvironmentVariableA("TRP_AMD_MEMORY_TRACE", enabled, 8) && enabled[0] == '1';
    void* pointer{}; int status = -7;
    CHECK(gpa("cuMemAlloc", &pointer, 2000, 0, &status) == 0 && status == 0);
    CHECK(pointer == reinterpret_cast<void*>(GetProcAddress(real, "cuMemAlloc")));
    unsigned legacy{}; CHECK(reinterpret_cast<int (*)(unsigned*, unsigned)>(pointer)(&legacy, 8) == 801);
    status = -7; CHECK(gpa("unknown", &pointer, 3020, 0, &status) == 500 && status == 1 && pointer == nullptr);
    auto create = reinterpret_cast<int (*)(void**, unsigned, int)>(GetProcAddress(gate, "cuCtxCreate_v2")); CHECK(create);
    void* ctx{}; CHECK(create(&ctx, 0, 1) == 0);
    CHECK(gpa("cuMemAlloc", &pointer, 3020, 0, &status) == 0 && status == 0);
    if (!tracing) CHECK(pointer == reinterpret_cast<void*>(GetProcAddress(real, "cuMemAlloc_v2")));
    unsigned long long allocation{}; CHECK(reinterpret_cast<int (*)(unsigned long long*, size_t)>(pointer)(&allocation, 64) == 0);
    SetEnvironmentVariableA("MOCK_VARIANT_RESULTS", "1");
    for (const char* symbol : {"cuMemsetD8", "cuMemsetD16", "cuMemsetD32", "cuMemsetD2D8", "cuMemsetD2D16", "cuMemsetD2D32"}) {
        CHECK(gpa(symbol, &pointer, 3020, 2, &status) == 0 && status == 0);
        const std::string exportName = std::string(symbol) + "_v2_ptds";
        if (!tracing) CHECK(pointer == reinterpret_cast<void*>(GetProcAddress(real, exportName.c_str())));
        const bool two = std::strstr(symbol, "D2D") != nullptr;
        int result{};
        if (std::strstr(symbol, "32")) {
            if (two) result = reinterpret_cast<int (*)(unsigned long long, size_t, unsigned, size_t, size_t)>(pointer)(allocation, 16, 0, 4, 1);
            else result = reinterpret_cast<int (*)(unsigned long long, unsigned, size_t)>(pointer)(allocation, 0, 4);
        } else if (std::strstr(symbol, "16")) {
            if (two) result = reinterpret_cast<int (*)(unsigned long long, size_t, unsigned short, size_t, size_t)>(pointer)(allocation, 16, 0, 4, 1);
            else result = reinterpret_cast<int (*)(unsigned long long, unsigned short, size_t)>(pointer)(allocation, 0, 4);
        } else {
            if (two) result = reinterpret_cast<int (*)(unsigned long long, size_t, unsigned char, size_t, size_t)>(pointer)(allocation, 16, 0, 4, 1);
            else result = reinterpret_cast<int (*)(unsigned long long, unsigned char, size_t)>(pointer)(allocation, 0, 4);
        }
        CHECK(result == 77);
    }
    CHECK(gpa("cuMemsetD8Async", &pointer, 3020, 2, &status) == 0);
    CHECK(reinterpret_cast<int (*)(unsigned long long, unsigned char, size_t, void*)>(pointer)(allocation, 0, 8, reinterpret_cast<void*>(0x123)) == 77);
    SetEnvironmentVariableA("MOCK_FAIL_FILL", "1");
    CHECK(gpa("cuMemsetD32", &pointer, 3020, 0, &status) == 0);
    CHECK(reinterpret_cast<int (*)(unsigned long long, unsigned, size_t)>(pointer)(allocation, 0, 4) == 700);
    CHECK(gpa("cuMemFree", &pointer, 3020, 0, &status) == 0);
    CHECK(reinterpret_cast<int (*)(unsigned long long)>(pointer)(allocation) == 0);
    SetEnvironmentVariableA("MOCK_FAIL_ALLOC", "1");
    CHECK(gpa("cuMemAlloc", &pointer, 3020, 0, &status) == 0);
    CHECK(reinterpret_cast<int (*)(unsigned long long*, size_t)>(pointer)(&allocation, 64) == 2);
    std::printf("PASS CPU gate: legacy ABI, modern lookup, PTDS/PTSZ, lookup status and exact error forwarding; tracing=%d\n", tracing);
    return 0;
}
