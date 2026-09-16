#pragma once

#include <Windows.h>

#include <cstdint>

namespace midpoint_fix
{
using LogCallback = void (*)(const wchar_t* message);

enum class AdapterKind { Unavailable, Ada, Other };

void SetLogCallback(LogCallback callback) noexcept;
AdapterKind ObserveD3D12Adapter(void* device) noexcept;
bool ObserveD3D12Device(void* device) noexcept;
bool PatchProvider(HMODULE module, const wchar_t* path) noexcept;
bool AdapterVerified() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;
}
