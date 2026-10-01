#pragma once

#include "SourceDLSSGInterop.h"

namespace TheosRenderPipeline::SourceDLSSG
{
	// Observation only. Never retries work, waits for completion, or releases
	// device/resource owners. The backend retains its original first error and
	// calls Report only for that first fault.
	class DeviceLossDiagnostics
	{
	public:
		// Missing file or setting reads as false and logs nothing.
		static bool ReadDREDSetting(const wchar_t* a_path) noexcept;
		// Before the host's D3D12 device creation. False leaves OS/other-owner
		// DRED settings alone. Enabling DRED affects subsequent process devices.
		void Configure(bool a_enableDRED) noexcept;
		// Reports only a GPU failure: an observed device removal or a fault
		// retained by the interop. Returns whether a report was written.
		bool Report(HRESULT a_result, const char* a_operation, std::uint64_t a_frame,
			ID3D11Device* a_device11, ID3D12Device* a_device12,
			const InteropFailureDiagnostics& a_interop) noexcept;

		// Shared production serializers, also exercised with synthetic DRED
		// lists. Runtime owns all pointed-to data throughout these calls.
		static void LogBreadcrumbs(const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1& a_data);
		static void LogPageFault(const D3D12_DRED_PAGE_FAULT_OUTPUT1& a_data);

	private:
		bool requested_{};
		bool configured_{};
	};
}
