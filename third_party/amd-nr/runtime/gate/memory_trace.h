#pragma once
#include <atomic>
#include <cstdint>

// Included after real()/logf()/g_lock. Diagnostic only: no context changes,
// synchronization, request mutation, recovery, or control-flow suppression.
namespace memory_trace {
inline bool enabled() {
    static const bool value = [] {
        char flag[8]{};
        return GetEnvironmentVariableA("TRP_AMD_MEMORY_TRACE", flag, sizeof(flag)) == 1 && flag[0] == '1';
    }();
    return value;
}
struct Snapshot {
    void* context{};
    int device{-1};
    int contextResult{-1}, deviceResult{-1};
    void* hipContext{};
    int hipDevice{-1};
    int hipContextResult{-1}, hipDeviceResult{-1};
};
inline Snapshot observe() {
    Snapshot s;
    static auto context = reinterpret_cast<int (*)(void**)>(real("cuCtxGetCurrent"));
    static auto device = reinterpret_cast<int (*)(int*)>(real("cuCtxGetDevice"));
    if (context) s.contextResult = context(&s.context);
    if (s.context && device) s.deviceResult = device(&s.device);
    HMODULE hip = GetModuleHandleW(L"amdhip64_7.dll");
    if (hip) {
        auto hc = reinterpret_cast<int (*)(void**)>(GetProcAddress(hip, "hipCtxGetCurrent"));
        auto hd = reinterpret_cast<int (*)(int*)>(GetProcAddress(hip, "hipGetDevice"));
        if (hc) s.hipContextResult = hc(&s.hipContext);
        if (hd) s.hipDeviceResult = hd(&s.hipDevice);
    }
    return s;
}
struct Allocation { unsigned long long pointer{}; size_t bytes{}; void* context{}; int device{-1}; };
inline Allocation allocations[4096]{};
inline bool tableOverflow{};
inline void allocated(unsigned long long pointer, size_t bytes, const Snapshot& s) {
    EnterCriticalSection(&g_lock);
    bool stored = false;
    for (auto& a : allocations) {
        if (!a.pointer || a.pointer == pointer) {
            a = {pointer, bytes, s.context, s.device}; stored = true; break;
        }
    }
    if (!stored) tableOverflow = true;
    LeaveCriticalSection(&g_lock);
}
inline void freed(unsigned long long pointer) {
    EnterCriticalSection(&g_lock);
    for (auto& a : allocations) if (a.pointer == pointer) a = {};
    LeaveCriticalSection(&g_lock);
}
inline std::atomic<unsigned long long> sequence{};
inline unsigned long long begin(const char* api, unsigned long long pointer, size_t count,
                                unsigned elementBytes, void* stream, const Snapshot& s) {
    const auto id = ++sequence;
    Allocation owner{};
    bool overflow{};
    EnterCriticalSection(&g_lock);
    for (const auto& a : allocations) {
        if (a.pointer && pointer >= a.pointer && pointer - a.pointer < a.bytes) { owner = a; break; }
    }
    overflow = tableOverflow;
    LeaveCriticalSection(&g_lock);
    logf("C10 memory begin id=%llu api=%s pointer=0x%llx count=%zu elementBytes=%u stream=%p CUDA-context=%p CUDA-device=%d CUDA-query=%d/%d HIP-context=%p HIP-device=%d HIP-query=%d/%d ownerBase=0x%llx ownerBytes=%zu ownerContext=%p ownerDevice=%d tableOverflow=%d\n",
        id, api, pointer, count, elementBytes, stream, s.context, s.device, s.contextResult, s.deviceResult,
        s.hipContext, s.hipDevice, s.hipContextResult, s.hipDeviceResult,
        owner.pointer, owner.bytes, owner.context, owner.device, overflow);
    return id;
}
inline void end(unsigned long long id, int result, unsigned long long pointer) {
    logf("C10 memory returned id=%llu result=%d pointer=0x%llx\n", id, result, pointer);
}
}
