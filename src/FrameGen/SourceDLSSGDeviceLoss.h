#pragma once

#include "SourceDLSSGInterop.h"

namespace TheosRenderPipeline::SourceDLSSG
{
	// How Backend::Check handles one checked result, kept apart from Backend so
	// the first-fault latch and single report are testable without Streamline.
	struct CheckRouting
	{
		bool latch{};            // First failure: record it, log it and Report it once.
		bool logExtendedWait{};  // The pending extended wait is not in that report.
	};
	// a_latched is the backend's current fault. a_extendedWait is the wait taken
	// for this check, or null when none was pending.
	CheckRouting RouteCheck(HRESULT a_result, HRESULT a_latched, const InteropFailureDiagnostics& a_interop,
		const RetirementWaitDiagnostics* a_extendedWait) noexcept;

	// What a failed guide copy (depth, motion or HUD-less colour into a texture
	// shared with D3D12) means for the presenter. A copy refused by its own
	// checks has written nothing and the frame is still whole. Latching that as
	// a fault refused every later Present: a frozen picture with the game
	// running behind it (a player's log, 0.6.4). Refused turns frame generation
	// and NR off instead and presentation continues. Device loss still latches.
	enum class GuideCopyRouting { Copied, Refused, Fault };
	// a_removedReason is the D3D11 device's GetDeviceRemovedReason. a_untouched:
	// every failure of this copy returns before it writes anything (the depth
	// copy). A copy that first waits on the GPU promises that only for
	// E_INVALIDARG; its other failures stay faults.
	GuideCopyRouting RouteGuideCopy(HRESULT a_result, HRESULT a_removedReason, bool a_untouched) noexcept;

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
