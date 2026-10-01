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
			DWORD saved{ GetLastError() };
			~LastErrorScope() { SetLastError(saved); }
		};
		constexpr UINT kMaxNodes = 32;
		constexpr UINT kHistorySize = 65536;
		constexpr UINT kMaxOperations = 16;

		std::string Name(const char* a_narrow, const wchar_t* a_wide)
		{
			std::string text;
			if (!a_narrow && !a_wide) { return "unnamed"; }
			for (std::size_t i = 0; i < 120; ++i) {
				const auto c = a_narrow ? static_cast<unsigned char>(a_narrow[i]) : static_cast<unsigned>(a_wide[i]);
				if (!c) { return text; }
				// Bound/sanitize diagnostic strings; preserve single-line logs.
				text += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
			}
			return text + "...";
		}

		bool IsDeviceLoss(HRESULT a_result)
		{
			return a_result == DXGI_ERROR_DEVICE_REMOVED || a_result == DXGI_ERROR_DEVICE_HUNG ||
				a_result == DXGI_ERROR_DEVICE_RESET || a_result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
		}

		const char* Reason(HRESULT a_result)
		{
			switch (a_result) {
			case S_OK: return "S_OK";
			case DXGI_ERROR_DEVICE_REMOVED: return "DEVICE_REMOVED";
			case DXGI_ERROR_DEVICE_HUNG: return "DEVICE_HUNG";
			case DXGI_ERROR_DEVICE_RESET: return "DEVICE_RESET";
			case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DRIVER_INTERNAL_ERROR";
			case DXGI_ERROR_INVALID_CALL: return "INVALID_CALL";
			default: return "other";
			}
		}

		const char* Breadcrumb(D3D12_AUTO_BREADCRUMB_OP a_operation)
		{
			switch (a_operation) {
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

		void Allocations(const D3D12_DRED_ALLOCATION_NODE1* a_node, const char* a_kind)
		{
			UINT count = 0;
			for (; a_node && count < kMaxNodes; a_node = a_node->pNext, ++count) {
				spdlog::info("[GPUFailure] DRED allocation kind={} index={} type={} object={} name={}",
					a_kind, count, static_cast<unsigned>(a_node->AllocationType), static_cast<const void*>(a_node->pObject),
					Name(a_node->ObjectNameA, a_node->ObjectNameW));
			}
			spdlog::info("[GPUFailure] DRED allocations kind={} count={} truncated={}", a_kind, count, a_node != nullptr);
		}
	}

	bool DeviceLossDiagnostics::ReadDREDSetting(const wchar_t* a_path) noexcept
	{
		LastErrorScope preserve;
		try {
			// An absent file is the normal configuration; say nothing about it.
			if (!a_path || GetFileAttributesW(a_path) == INVALID_FILE_ATTRIBUTES) { return false; }
			CSimpleIniA ini;
			ini.SetUnicode();
			const auto result = ini.LoadFile(a_path);
			if (result < 0) {
				spdlog::warn("[Diagnostics] TheosRenderPipeline.Diagnostics.ini unreadable readResult={}; DRED stays off",
					static_cast<int>(result));
				return false;
			}
			const bool enabled = ini.GetBoolValue("DeviceLoss", "EnableDRED", false);
			spdlog::info("[Diagnostics] TheosRenderPipeline.Diagnostics.ini EnableDRED={}; restart required for changes", enabled);
			return enabled;
		} catch (...) { return false; }
	}

	void DeviceLossDiagnostics::Configure(bool a_enableDRED) noexcept
	{
		LastErrorScope preserve;
		requested_ = a_enableDRED;
		if (!a_enableDRED) { return; }  // Existing process settings unchanged.
		try {
			Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
			const auto result = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
			bool contexts = false;
			if (SUCCEEDED(result) && settings) {
				settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				configured_ = true;
				// DRED 1.2: marker/event strings are a separate opt-in. Older
				// runtimes lack this interface; breadcrumbs still apply.
				Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings1;
				if (SUCCEEDED(settings.As(&settings1)) && settings1) {
					settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
					contexts = true;
				}
			}
			spdlog::info("[Diagnostics] DRED requested=true configured={} breadcrumbContexts={} settingsResult=0x{:08X}; applies to subsequently created D3D12 devices",
				configured_, contexts, static_cast<std::uint32_t>(result));
		} catch (...) {}  // Diagnostics cannot turn a supported startup into a failure.
	}

	void DeviceLossDiagnostics::LogBreadcrumbs(const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1& a_data)
	{
		auto* node = a_data.pHeadAutoBreadcrumbNode;
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

	void DeviceLossDiagnostics::LogPageFault(const D3D12_DRED_PAGE_FAULT_OUTPUT1& a_data)
	{
		spdlog::info("[GPUFailure] DRED pageFaultVA=0x{:016X}", a_data.PageFaultVA);
		Allocations(a_data.pHeadExistingAllocationNode, "existing");
		Allocations(a_data.pHeadRecentFreedAllocationNode, "recently-freed");
	}

	bool DeviceLossDiagnostics::Report(HRESULT a_result, const char* a_operation, std::uint64_t a_frame,
		ID3D11Device* a_device11, ID3D12Device* a_device12, const InteropFailureDiagnostics& a_interop) noexcept
	{
		LastErrorScope preserve;
		if (SUCCEEDED(a_result)) { return false; }
		try {
			const auto reason11 = a_device11 ? a_device11->GetDeviceRemovedReason() : S_OK;
			const auto reason12 = a_device12 ? a_device12->GetDeviceRemovedReason() : S_OK;
			const bool removed = IsDeviceLoss(a_result) || FAILED(reason11) || FAILED(reason12);
			// Startup, configuration and validation failures keep their own error line.
			if (!removed && !a_interop.valid) { return false; }
			spdlog::error("[GPUFailure] first failure operation={} sessionFrame={} result=0x{:08X} ({}) device11={} reason11Available={} reason11=0x{:08X} ({}) device12={} reason12Available={} reason12=0x{:08X} ({}) DREDRequested={} DREDConfigured={}",
				a_operation ? a_operation : "unknown", a_frame, static_cast<std::uint32_t>(a_result), Reason(a_result),
				static_cast<void*>(a_device11), a_device11 != nullptr, static_cast<std::uint32_t>(reason11), Reason(reason11),
				static_cast<void*>(a_device12), a_device12 != nullptr, static_cast<std::uint32_t>(reason12), Reason(reason12),
				requested_, configured_);
			spdlog::info("[GPUFailure] retainedInteropFailure={} stage={} work={} slot={} fenceValue={} submitted={} completedAvailable={} result=0x{:08X}",
				a_interop.valid, a_interop.stage, WorkName(a_interop.work), a_interop.slot, a_interop.fenceValue,
				a_interop.submitted, a_interop.completedAvailable, static_cast<std::uint32_t>(a_interop.result));
			if (const auto& wait = a_interop.wait; a_interop.valid && wait.target && a_interop.waitPerformed) {
				// The retirement wait's own record; the backend omits its separate
				// extended-wait line when this report carries the same wait.
				spdlog::info("[GPUFailure] retirement wait work={} target={} completed={}->{} waitPerformed=true waitResult=0x{:08X} elapsedMs={} slices={} sliceMs={} stallLimitMs={} result=0x{:08X}",
					WorkName(wait.work), wait.target, wait.completedAtStart, wait.completedAtEnd,
					a_interop.waitResult, wait.elapsedMs, wait.slices, a_interop.waitPolicy.sliceMs,
					a_interop.waitPolicy.stallLimitMs, static_cast<std::uint32_t>(wait.result));
			} else if (a_interop.valid && wait.target) {
				// Failed before any wait slice ran; there is no wait result to report.
				spdlog::info("[GPUFailure] retirement wait work={} target={} completed={} waitPerformed=false",
					WorkName(wait.work), wait.target, wait.completedAtStart);
			}
			if (a_device12 && removed) {
				Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
				const auto query = a_device12->QueryInterface(IID_PPV_ARGS(&dred));
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
			} else {
				spdlog::info("[GPUFailure] DRED not queried: device12Available={} removalObserved={}", a_device12 != nullptr, removed);
			}
			spdlog::info("[GPUFailure] diagnostic complete; original failure and resource retirement unchanged; cached runtime state is not recovery");
			spdlog::default_logger()->flush();
		} catch (...) {
			try {
				spdlog::warn("[GPUFailure] diagnostic incomplete; original failure retained");
				spdlog::default_logger()->flush();
			} catch (...) {}
		}
		return true;
	}
}
