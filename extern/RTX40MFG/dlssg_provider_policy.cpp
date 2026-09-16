#include "dlssg_provider_policy.h"

namespace dlssg_provider_policy
{
bool IsDlssgImplementationModule(HMODULE module) noexcept
{
    return module && HasDlssgExportIdentity(
        GetProcAddress(module, kD3d12ImplementationExport) != nullptr,
        GetProcAddress(module, kDirectSrImplementationExport) != nullptr);
}
}
