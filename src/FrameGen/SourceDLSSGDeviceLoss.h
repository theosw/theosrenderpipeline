#pragma once

#include "SourceDLSSGInterop.h"
#include <atomic>

namespace TheosRenderPipeline::SourceDLSSG
{
    // Observation only. Never retries work, waits for completion, or releases
    // device/resource owners. The backend retains its original first error.
    class DeviceLossDiagnostics
    {
    public:
        static bool ReadDREDSetting(const wchar_t* path =
            L"Data\\SKSE\\Plugins\\TheosRenderPipeline.Diagnostics.ini") noexcept;
        // Before the host's D3D12 device creation. False leaves OS/other-owner
        // DRED settings alone. Enabling DRED affects subsequent process devices.
        void Configure(bool enableDRED) noexcept;
        bool Report(HRESULT result, const char* operation, std::uint64_t frame,
            ID3D11Device* device11, ID3D12Device* device12,
            const InteropFailureDiagnostics& interop) noexcept;

        // Shared production serializers, also exercised with synthetic DRED
        // lists. Runtime owns all pointed-to data throughout these calls.
        static void LogBreadcrumbs(const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1& data);
        static void LogPageFault(const D3D12_DRED_PAGE_FAULT_OUTPUT1& data);

    private:
        bool requested_{};
        bool configured_{};
        std::atomic_bool reported_{};
    };
}
