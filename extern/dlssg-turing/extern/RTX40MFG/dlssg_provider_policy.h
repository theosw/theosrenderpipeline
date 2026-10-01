#pragma once
#include <Windows.h>

namespace dlssg_provider_policy
{
inline constexpr char kD3d12ImplementationExport[] =
    "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl";
inline constexpr char kDirectSrImplementationExport[] = "NVSDK_NGX_DirectSR_Create";

constexpr bool HasDlssgExportIdentity(bool populateDeviceParameters, bool directSr) noexcept
{
    return populateDeviceParameters && !directSr;
}

// Program discovery determines patch compatibility, independently of file version.
bool IsDlssgImplementationModule(HMODULE module) noexcept;
}
