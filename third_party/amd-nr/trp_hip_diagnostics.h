#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace TRPAmdDiagnostics {
// Observe the already-loaded HIP runtime. Never load/set a device or context.
struct HipSnapshot {
    void* context{};
    int device{-1}, contextResult{-1}, deviceResult{-1}, luidResult{-1}, pciResult{-1};
    std::uint64_t luid{};
    unsigned nodeMask{};
    char pci[64]{};
};
inline HipSnapshot CurrentHip() {
    HipSnapshot s;
    auto hip = GetModuleHandleW(L"amdhip64_7.dll");
    if (!hip) hip = GetModuleHandleW(L"amdhip64_6.dll");
    if (!hip) hip = GetModuleHandleW(L"amdhip64.dll");
    if (!hip) return s;
    using Context = int (*)(void**);
    using Device = int (*)(int*);
    using Luid = int (*)(char*, unsigned*, int);
    using PCI = int (*)(char*, int, int);
    auto context = reinterpret_cast<Context>(GetProcAddress(hip, "hipCtxGetCurrent"));
    auto device = reinterpret_cast<Device>(GetProcAddress(hip, "hipGetDevice"));
    auto luid = reinterpret_cast<Luid>(GetProcAddress(hip, "hipDeviceGetLuid"));
    auto pci = reinterpret_cast<PCI>(GetProcAddress(hip, "hipDeviceGetPCIBusId"));
    if (context) s.contextResult = context(&s.context);
    if (device) s.deviceResult = device(&s.device);
    if (s.deviceResult == 0) {
        char bytes[8]{};
        if (luid) s.luidResult = luid(bytes, &s.nodeMask, s.device);
        if (s.luidResult == 0) std::memcpy(&s.luid, bytes, sizeof(s.luid));
        if (pci) s.pciResult = pci(s.pci, sizeof(s.pci), s.device);
    }
    return s;
}
}
