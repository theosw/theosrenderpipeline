// Standalone allocation/fill diagnostic; no Skyrim, NGX, image interop or kernels.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "../include/trp_diagnostics.h"

#ifndef TRP_PROBE_SOURCE
#define TRP_PROBE_SOURCE "unidentified"
#endif
static_assert(sizeof(void*) == 8, "The diagnostic CUDA ABIs require x64");
namespace {
FILE* output{};
void log(const char* format, ...) {
    char text[2048]{}; va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args); va_end(args);
    char prefix[160]{}; TRPAmdDiagnostics::Prefix(prefix, sizeof(prefix));
    std::printf("%s%s\n", prefix, text); std::fflush(stdout);
    if (output) { std::fprintf(output, "%s%s\n", prefix, text); std::fflush(output); }
}
LONG WINAPI fault(EXCEPTION_POINTERS* info) {
    log("FATAL exception=0x%08lx address=%p thread=%lu; process will terminate",
        info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    HANDLE file = CreateFileW(L"memory-probe.dmp", GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &exception, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
std::wstring exe_root() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
    std::wstring result(path); return result.substr(0, result.find_last_of(L"\\/"));
}
void describe(HMODULE module, const char* label) {
    wchar_t path[32768]{};
    if (!module || !GetModuleFileNameW(module, path, 32768)) { log("module=%s unavailable", label); return; }
    char utf8[65536]{}; WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
    DWORD ignored{}, bytes = GetFileVersionInfoSizeW(path, &ignored);
    std::vector<unsigned char> data(bytes); VS_FIXEDFILEINFO* version{}; unsigned size{};
    if (bytes && GetFileVersionInfoW(path, 0, bytes, data.data()) &&
        VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &size) && size >= sizeof(*version))
        log("module=%s path=%s version=%u.%u.%u.%u", label, utf8,
            HIWORD(version->dwFileVersionMS), LOWORD(version->dwFileVersionMS),
            HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS));
    else log("module=%s path=%s version=unavailable", label, utf8);
}
template<typename Fn> Fn api(HMODULE module, const char* name) {
    auto result = reinterpret_cast<Fn>(GetProcAddress(module, name));
    if (!result) log("missing export %s", name);
    return result;
}
bool ok(int result, const char* operation) {
    log("%s returned=%d", operation, result); return result == 0;
}
}

int main(int argc, char** argv) {
    std::string variant = "d8", lookup = "export";
    unsigned long long bytes = 147719680;
    int requested = -1; bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--list") list = true;
        else if (i + 1 < argc && arg == "--case") variant = argv[++i];
        else if (i + 1 < argc && arg == "--lookup") lookup = argv[++i];
        else if (i + 1 < argc && arg == "--device") requested = std::stoi(argv[++i]);
        else if (i + 1 < argc && arg == "--bytes") bytes = std::stoull(argv[++i]);
        else { std::printf("Usage: --list | --case d8|d16|d32|a8|a16|a32 --lookup export|gpa [--device ordinal] [--bytes count]\n"); return 2; }
    }
    if ((lookup != "export" && lookup != "gpa") || bytes == 0 || bytes > 147719680 || bytes % 4 != 0 ||
        (variant != "d8" && variant != "d16" && variant != "d32" && variant != "a8" && variant != "a16" && variant != "a32")) return 2;
    output = std::fopen("memory-probe.log", "wx");
    if (!output) { std::printf("Use a fresh writable working directory for each probe run.\n"); return 2; }
    SetUnhandledExceptionFilter(fault);
    log("C10 memory probe source=%s session=%s case=%s lookup=%s bytes=%llu", TRP_PROBE_SOURCE,
        TRPAmdDiagnostics::Session(), variant.c_str(), lookup.c_str(), bytes);
    SetEnvironmentVariableA("TRP_AMD_MEMORY_TRACE", "1");
    SetEnvironmentVariableW(L"DLSS5NR_GATE_LOG", L"memory-gate.log");
    const auto root = exe_root() + L"\\runtime";
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    AddDllDirectory(root.c_str());
    HMODULE real = LoadLibraryExW((root + L"\\zluda_real.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!real) { log("ZLUDA load failed win32=%lu; installed compatible HIP is required", GetLastError()); return 2; }
    HMODULE cuda = LoadLibraryExW((root + L"\\nvcuda.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!cuda) { log("trace gate load failed win32=%lu", GetLastError()); return 2; }
    describe(real, "ZLUDA"); describe(cuda, "CUDA gate");
    auto init = api<int (*)(unsigned)>(cuda, "cuInit");
    auto count = api<int (*)(int*)>(cuda, "cuDeviceGetCount");
    auto get = api<int (*)(int*, int)>(cuda, "cuDeviceGet");
    auto name = api<int (*)(char*, int, int)>(cuda, "cuDeviceGetName");
    auto luid = api<int (*)(char*, unsigned*, int)>(cuda, "cuDeviceGetLuid");
    auto pci = api<int (*)(char*, int, int)>(cuda, "cuDeviceGetPCIBusId");
    auto create = api<int (*)(void**, unsigned, int)>(cuda, "cuCtxCreate_v2");
    auto destroy = api<int (*)(void*)>(cuda, "cuCtxDestroy_v2");
    auto current = api<int (*)(void**)>(cuda, "cuCtxGetCurrent");
    auto device = api<int (*)(int*)>(cuda, "cuCtxGetDevice");
    auto sync = api<int (*)()>(cuda, "cuCtxSynchronize");
    auto streamCreate = api<int (*)(void**, unsigned)>(cuda, "cuStreamCreate");
    auto streamDestroy = api<int (*)(void*)>(cuda, "cuStreamDestroy_v2");
    auto streamSync = api<int (*)(void*)>(cuda, "cuStreamSynchronize");
    auto copy = api<int (*)(void*, unsigned long long, size_t)>(cuda, "cuMemcpyDtoH_v2");
    if (!init || !count || !get || !name || !luid || !pci || !create || !destroy || !current || !device || !sync ||
        !streamCreate || !streamDestroy || !streamSync || !copy || !ok(init(0), "cuInit")) return 2;
    describe(GetModuleHandleW(L"amdhip64_7.dll"), "HIP");
    unsigned long long expectedLuid{}; size_t largest{};
    IDXGIFactory1* factory{};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1* adapter{};
        for (unsigned i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
            DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
            if (desc.VendorId == 0x1002 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.DedicatedVideoMemory > largest) {
                largest = desc.DedicatedVideoMemory; std::memcpy(&expectedLuid, &desc.AdapterLuid, 8);
            }
            adapter->Release();
        }
        factory->Release();
    }
    int n{}; if (!ok(count(&n), "device count")) return 2;
    int selected = -1, selectedDevice = -1;
    for (int ordinal = 0; ordinal < n; ++ordinal) {
        int dev{}; char text[256]{}, bus[64]{}, id[8]{}; unsigned mask{}; unsigned long long id64{};
        if (!ok(get(&dev, ordinal), "cuDeviceGet")) return 2;
        if (name(text, sizeof(text), dev) != 0) return 2;
        const auto lr = luid(id, &mask, dev); const auto pr = pci(bus, sizeof(bus), dev); std::memcpy(&id64, id, 8);
        log("device ordinal=%d handle=%d name=%s PCI=%s PCI-result=%d LUID=%016llx LUID-result=%d nodeMask=%u",
            ordinal, dev, text, bus, pr, id64, lr, mask);
        const bool amd = std::strstr(text, "AMD") || std::strstr(text, "Radeon");
        if (amd && ((requested >= 0 && ordinal == requested) || (requested < 0 && expectedLuid && lr == 0 && id64 == expectedLuid))) {
            selected = ordinal; selectedDevice = dev;
        }
    }
    if (list) { log("LIST COMPLETE; no allocations/fills performed"); return 0; }
    if (selected < 0) { log("No selected AMD adapter; default matches largest discrete AMD DXGI LUID. Use --list to inspect."); return 2; }
    void* context{};
    log("context create begin ordinal=%d", selected);
    if (!ok(create(&context, 0, selectedDevice), "cuCtxCreate_v2")) return 2;
    void* observed{}; int observedDevice{-1};
    if (!ok(current(&observed), "cuCtxGetCurrent") || !ok(device(&observedDevice), "cuCtxGetDevice") ||
        observed != context || observedDevice != selectedDevice) { log("Context/device admission failed"); return 2; }
    log("context=%p device=%d admitted", observed, observedDevice);
    using Alloc = int (*)(unsigned long long*, size_t);
    using Free = int (*)(unsigned long long);
    auto alloc = api<Alloc>(cuda, "cuMemAlloc_v2"); auto free = api<Free>(cuda, "cuMemFree_v2");
    const bool asynchronous = variant[0] == 'a'; const int bits = std::stoi(variant.substr(1));
    const std::string base = "cuMemsetD" + std::to_string(bits) + (asynchronous ? "Async" : "");
    void* fill = reinterpret_cast<void*>(GetProcAddress(cuda, (base + (asynchronous ? "" : "_v2")).c_str()));
    if (lookup == "gpa") {
        auto resolve = api<int (*)(const char*, void**, int, unsigned long long)>(cuda, "cuGetProcAddress");
        void *a{}, *f{};
        if (!resolve || !ok(resolve("cuMemAlloc", &a, 3020, 0), "resolve alloc") || !a ||
            !ok(resolve("cuMemFree", &f, 3020, 0), "resolve free") || !f ||
            !ok(resolve(base.c_str(), &fill, 3020, 0), "resolve fill") || !fill) return 3;
        alloc = reinterpret_cast<Alloc>(a); free = reinterpret_cast<Free>(f);
    }
    if (!alloc || !free || !fill) { log("case unsupported: required memory entry missing"); return 3; }
    unsigned long long pointer{}; void* stream{};
    log("allocation begin bytes=%llu", bytes);
    if (!ok(alloc(&pointer, static_cast<size_t>(bytes)), "allocation") || !pointer) return 2;
    if (asynchronous && !ok(streamCreate(&stream, 1), "stream create")) return 2;
    // Zero matches the first suspected model initialization; the second pattern
    // verifies that fill/readback actually changes data, across the full buffer.
    for (unsigned pattern : {0u, 0xa5a5a5a5u}) {
        log("fill begin pointer=0x%llx bytes=%llu elementBits=%d pattern=0x%x stream=%p", pointer, bytes, bits, pattern, stream);
        int result = -1; const size_t elements = static_cast<size_t>(bytes / (bits / 8));
        if (asynchronous) {
            if (bits == 8) result = reinterpret_cast<int (*)(unsigned long long, unsigned char, size_t, void*)>(fill)(pointer, static_cast<unsigned char>(pattern), elements, stream);
            if (bits == 16) result = reinterpret_cast<int (*)(unsigned long long, unsigned short, size_t, void*)>(fill)(pointer, static_cast<unsigned short>(pattern), elements, stream);
            if (bits == 32) result = reinterpret_cast<int (*)(unsigned long long, unsigned, size_t, void*)>(fill)(pointer, pattern, elements, stream);
        } else {
            if (bits == 8) result = reinterpret_cast<int (*)(unsigned long long, unsigned char, size_t)>(fill)(pointer, static_cast<unsigned char>(pattern), elements);
            if (bits == 16) result = reinterpret_cast<int (*)(unsigned long long, unsigned short, size_t)>(fill)(pointer, static_cast<unsigned short>(pattern), elements);
            if (bits == 32) result = reinterpret_cast<int (*)(unsigned long long, unsigned, size_t)>(fill)(pointer, pattern, elements);
        }
        if (!ok(result, "fill")) return result == 801 ? 3 : 2;
        log("completion wait begin");
        if (!ok(asynchronous ? streamSync(stream) : sync(), "completion wait")) return 2;
        std::vector<unsigned char> data(static_cast<size_t>(bytes));
        log("full readback begin bytes=%llu", bytes);
        if (!ok(copy(data.data(), pointer, data.size()), "full readback")) return 2;
        if (!std::all_of(data.begin(), data.end(), [pattern](unsigned char value) { return value == static_cast<unsigned char>(pattern); })) {
            log("FAIL readback mismatch"); return 2;
        }
        log("full readback VERIFIED bytes=%llu pattern=0x%x", bytes, pattern);
    }
    if (stream && !ok(streamDestroy(stream), "stream destroy")) return 2;
    if (!ok(free(pointer), "free") || !ok(destroy(context), "context destroy")) return 2;
    log("PASS allocation/fill/readback only; model/Skyrim acceptance remains untested");
    return 0;
}
