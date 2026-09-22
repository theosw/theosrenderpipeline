#include "SourceDLSSGDeviceLoss.h"
#include <SimpleIni.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <string>

namespace TheosRenderPipeline::SourceDLSSG
{
    namespace
    {
        struct LastErrorScope
        {
            DWORD saved{GetLastError()};
            ~LastErrorScope() { SetLastError(saved); }
        };
        constexpr UINT kMaxNodes = 32;
        constexpr UINT kHistorySize = 65536;
        constexpr UINT kMaxOperations = 16;

        std::string Name(const char* narrow, const wchar_t* wide)
        {
            std::string text;
            if (!narrow && !wide) { return "unnamed"; }
            for (std::size_t i = 0; i < 120; ++i) {
                const auto c = narrow ? static_cast<unsigned char>(narrow[i]) : static_cast<unsigned>(wide[i]);
                if (!c) { return text; }
                // Bound/sanitize diagnostic strings; preserve single-line logs.
                text += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
            }
            return text + "...";
        }

        const char* Reason(HRESULT result)
        {
            switch (result) {
            case S_OK: return "S_OK";
            case DXGI_ERROR_DEVICE_REMOVED: return "DEVICE_REMOVED";
            case DXGI_ERROR_DEVICE_HUNG: return "DEVICE_HUNG";
            case DXGI_ERROR_DEVICE_RESET: return "DEVICE_RESET";
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DRIVER_INTERNAL_ERROR";
            case DXGI_ERROR_INVALID_CALL: return "INVALID_CALL";
            default: return "other";
            }
        }

        const char* Breadcrumb(D3D12_AUTO_BREADCRUMB_OP operation)
        {
            switch (operation) {
            case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
            case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
            case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
            case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
            case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
            case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
            case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
            case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
            case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
            default: return "other";
            }
        }

        void Allocations(const D3D12_DRED_ALLOCATION_NODE1* node, const char* kind)
        {
            UINT count = 0;
            for (; node && count < kMaxNodes; node = node->pNext, ++count) {
                spdlog::info("[GPUFailure] DRED allocation kind={} index={} type={} object={} name={}",
                    kind, count, static_cast<unsigned>(node->AllocationType), static_cast<const void*>(node->pObject),
                    Name(node->ObjectNameA, node->ObjectNameW));
            }
            spdlog::info("[GPUFailure] DRED allocations kind={} count={} truncated={}", kind, count, node != nullptr);
        }
    }

    bool DeviceLossDiagnostics::ReadDREDSetting(const wchar_t* path) noexcept
    {
        LastErrorScope preserve;
        try {
            CSimpleIniA ini;
            ini.SetUnicode();
            const auto result = ini.LoadFile(path);
            const bool enabled = result >= 0 && ini.GetBoolValue("DeviceLoss", "EnableDRED", false);
            spdlog::info("[GPUFailure] startup diagnostic INI readResult={} EnableDRED={}; restart required for changes",
                static_cast<int>(result), enabled);
            return enabled;
        } catch (...) { return false; }
    }

    void DeviceLossDiagnostics::Configure(bool enableDRED) noexcept
    {
        LastErrorScope preserve;
        requested_ = enableDRED;
        try {
            if (!enableDRED) {
                spdlog::info("[GPUFailure] DRED not requested; existing process settings unchanged");
                return;
            }
            Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
            const auto result = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
            if (SUCCEEDED(result) && settings) {
                settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                configured_ = true;
            }
            spdlog::info("[GPUFailure] DRED requested=true configured={} settingsResult=0x{:08X}; applies to subsequently created D3D12 devices",
                configured_, static_cast<std::uint32_t>(result));
        } catch (...) {} // Diagnostics cannot turn a supported startup into a failure.
    }

    void DeviceLossDiagnostics::LogBreadcrumbs(const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1& data)
    {
        auto* node = data.pHeadAutoBreadcrumbNode;
        UINT nodes = 0;
        for (; node && nodes < kMaxNodes; node = node->pNext, ++nodes) {
            const bool hasCompleted = node->pLastBreadcrumbValue != nullptr;
            const UINT completed = hasCompleted ? *node->pLastBreadcrumbValue : 0;
            const UINT count = node->BreadcrumbCount;
            spdlog::info("[GPUFailure] DRED list={} queue={} listObject={} queueObject={} completedAvailable={} completed={} count={} historyAvailable={} invalidCompleted={}",
                Name(node->pCommandListDebugNameA, node->pCommandListDebugNameW),
                Name(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW),
                static_cast<const void*>(node->pCommandList), static_cast<const void*>(node->pCommandQueue),
                hasCompleted, completed, count, node->pCommandHistory != nullptr, hasCompleted && completed > count);
            if (hasCompleted && completed <= count && node->pCommandHistory) {
                // DRED retains at most the last 64K operations in a ring. Never
                // treat completed as a pointer index or read overwritten entries.
                const UINT firstRetained = count > kHistorySize ? count - kHistorySize : 0;
                const UINT start = (std::max)(firstRetained, completed > 8 ? completed - 8 : 0);
                const UINT end = start + (std::min)(kMaxOperations, count - start);
                for (UINT index = start; index < end; ++index) {
                    const auto op = node->pCommandHistory[index % kHistorySize];
                    spdlog::info("[GPUFailure] DRED op index={} code={} name={} completed={}",
                        index, static_cast<unsigned>(op), Breadcrumb(op), index < completed);
                }
            }
            if (node->pBreadcrumbContexts) {
                const auto limit = (std::min)(node->BreadcrumbContextsCount, kMaxOperations);
                for (UINT i = 0; i < limit; ++i) {
                    const auto& context = node->pBreadcrumbContexts[i];
                    spdlog::info("[GPUFailure] DRED context index={} text={}", context.BreadcrumbIndex,
                        Name(nullptr, context.pContextString));
                }
                if (limit < node->BreadcrumbContextsCount) { spdlog::info("[GPUFailure] DRED contexts truncated=true"); }
            }
        }
        spdlog::info("[GPUFailure] DRED breadcrumbLists={} truncated={}", nodes, node != nullptr);
    }

    void DeviceLossDiagnostics::LogPageFault(const D3D12_DRED_PAGE_FAULT_OUTPUT1& data)
    {
        spdlog::info("[GPUFailure] DRED pageFaultVA=0x{:016X}", data.PageFaultVA);
        Allocations(data.pHeadExistingAllocationNode, "existing");
        Allocations(data.pHeadRecentFreedAllocationNode, "recently-freed");
    }

    bool DeviceLossDiagnostics::Report(HRESULT result, const char* operation, std::uint64_t frame,
        ID3D11Device* device11, ID3D12Device* device12, const InteropFailureDiagnostics& interop) noexcept
    {
        LastErrorScope preserve;
        if (SUCCEEDED(result) || reported_.exchange(true)) { return false; }
        try {
            const auto reason11 = device11 ? device11->GetDeviceRemovedReason() : S_OK;
            const auto reason12 = device12 ? device12->GetDeviceRemovedReason() : S_OK;
            spdlog::error("[GPUFailure] first failure operation={} sessionFrame={} result=0x{:08X} ({}) device11={} reason11Available={} reason11=0x{:08X} ({}) device12={} reason12Available={} reason12=0x{:08X} ({}) DREDRequested={} DREDConfigured={}",
                operation ? operation : "unknown", frame, static_cast<std::uint32_t>(result), Reason(result),
                static_cast<void*>(device11), device11 != nullptr, static_cast<std::uint32_t>(reason11), Reason(reason11),
                static_cast<void*>(device12), device12 != nullptr, static_cast<std::uint32_t>(reason12), Reason(reason12), requested_, configured_);
            spdlog::info("[GPUFailure] retainedInteropFailure={} stage={} work={} slot={} fenceValue={} submitted={} waitTarget={} completedAvailable={} completed={} timeoutMs={} waitPerformed={} waitResult=0x{:08X} result=0x{:08X}",
                interop.valid, interop.stage, WorkName(interop.work), interop.slot, interop.fenceValue,
                interop.submitted, interop.waitTarget, interop.completedAvailable, interop.completed, interop.timeoutMs,
                interop.waitPerformed, interop.waitResult, static_cast<std::uint32_t>(interop.result));
            const bool removed = FAILED(reason11) || FAILED(reason12) || result == DXGI_ERROR_DEVICE_REMOVED ||
                result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
            if (device12 && removed) {
                Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
                const auto query = device12->QueryInterface(IID_PPV_ARGS(&dred));
                spdlog::info("[GPUFailure] DRED interfaceResult=0x{:08X}", static_cast<std::uint32_t>(query));
                if (SUCCEEDED(query) && dred) {
                    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
                    D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
                    const auto breadcrumbResult = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
                    const auto pageFaultResult = dred->GetPageFaultAllocationOutput1(&pageFault);
                    spdlog::info("[GPUFailure] DRED breadcrumbsResult=0x{:08X} pageFaultResult=0x{:08X}; absent data does not exclude a GPU fault",
                        static_cast<std::uint32_t>(breadcrumbResult), static_cast<std::uint32_t>(pageFaultResult));
                    if (SUCCEEDED(breadcrumbResult)) { LogBreadcrumbs(breadcrumbs); }
                    if (SUCCEEDED(pageFaultResult)) { LogPageFault(pageFault); }
                }
            } else { spdlog::info("[GPUFailure] DRED not queried: device12Available={} removalObserved={}", device12 != nullptr, removed); }
            spdlog::info("[GPUFailure] diagnostic complete; original failure and resource retirement unchanged; cached runtime state is not recovery");
            spdlog::default_logger()->flush();
        } catch (...) {
            try { spdlog::warn("[GPUFailure] diagnostic incomplete; original failure retained"); spdlog::default_logger()->flush(); }
            catch (...) {}
        }
        return true;
    }
}
