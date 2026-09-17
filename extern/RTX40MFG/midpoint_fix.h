#pragma once

#include <Windows.h>

#include <cstdint>

namespace midpoint_fix
{
using LogCallback = void (*)(const wchar_t* message);

enum class AdapterKind { Unavailable, Ada, Other, Ampere };

// Build a private temporal clone before changing any provider input. Ownership
// transfers to the process-resident Ampere startup transaction. This does not
// admit hardware or publish a pointer and does not change the Ada owner.
struct AmpereTemporalClone {
    void* allocation{};
    uintptr_t slot{}, originalDescriptor{}, replacementDescriptor{};
    uint32_t outputBytes{};
};
bool BuildAmpereTemporalClone(HMODULE module, AmpereTemporalClone& output) noexcept;

void SetLogCallback(LogCallback callback) noexcept;
AdapterKind ObserveD3D12Adapter(void* device) noexcept;
bool ObserveD3D12Device(void* device) noexcept;
bool PatchProvider(HMODULE module, const wchar_t* path) noexcept;
bool AdapterVerified() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;
}
