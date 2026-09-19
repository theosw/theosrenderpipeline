#include <PCH.h>
#include "SourceDLSSGMFG.h"
#include "../PluginPaths.h"
#include "SourceDLSSGMFGPatch.h"
#include "../../extern/RTX40MFG/midpoint_fix.h"
#include "../../extern/RTX40MFG/dlssg_provider_policy.h"
#include "../../extern/MFGAmpere/runtime.hpp"
#include <d3d12.h>
#include <cstring>
#include <spdlog/spdlog.h>

namespace TheosRenderPipeline::SourceDLSSG
{
	void MFGUnlock::Configure(bool requested)
	{
		if (!started_) { state_.requested = requested; }
	}
	namespace
	{
		// Resolve only the configured bundle; the backend owns loading and calls.
		HMODULE RetainConfiguredModule(const std::filesystem::path& directory, const wchar_t* name)
		{
			return PluginPaths::RetainLoadedModule(directory / name);
		}
	}
	[[noreturn]] void MFGUnlock::Fail(const char* reason)
	{
		state_.failed = true; state_.status = reason;
		const auto guidance = state_.UsesAmpereUnlock() ?
			std::string("RTX 30-series frame generation requires the Full compatibility path.\n"
				"Keep SourceDLSSGMFGUnlock=true and include TheosRenderPipeline.log when reporting this startup failure.\n") :
			std::format("Temporal error: {}. Attempts: {}.\n\n"
				"Set SourceDLSSGMFGUnlock=false in SKSE/Plugins/TheosRenderPipeline.ini to use the unmodified NVIDIA runtime.\n",
				state_.temporalFailure, state_.attempts);
		util::report_and_fail(std::format(
			"Theo's Render Pipeline: MFG startup or patch verification failed.\n\n"
			"{}\n\n{}"
			"See TheosRenderPipeline.log for details. Skyrim will close after this message.", reason, guidance));
	}
	void MFGUnlock::EnterStartupScope() noexcept { trp::ampere::EnterStartupScope(); }
	void MFGUnlock::LeaveStartupScope() noexcept { trp::ampere::LeaveStartupScope(); }
	void MFGUnlock::BeforeStreamline(ID3D12Device* device, const std::filesystem::path& directory)
	{
		if (state_.route != MFGRoute::Unselected) { Fail("MFG startup adapter selection was repeated"); }
		state_.status = "selecting NVIDIA MFG path";
		midpoint_fix::SetLogCallback([](const wchar_t* text) {
			// Upstream callback is noexcept; diagnostics must not terminate the game.
			try { spdlog::info("[SourceDLSSG MFG] {}", std::filesystem::path(text).string()); } catch (...) {}
		});
		const auto adapter = state_.requested ? midpoint_fix::ObserveD3D12Adapter(device) : midpoint_fix::AdapterKind::Unavailable;
		if (!state_.SelectRoute(adapter)) {
			state_.temporalFailure = midpoint_fix::FailureCode();
			Fail("active rendering adapter could not be identified; no patches applied");
		}
		spdlog::info("[SourceDLSSG MFG] selected route={} compatibilityRequested={}",
			state_.UsesAmpereUnlock() ? "Ampere" : state_.UsesAdaUnlock() ? "Ada" : "Native", state_.requested);
		if (state_.UsesAmpereUnlock()) {
			if (!trp::ampere::Start(device, directory, [](const char* message) { spdlog::info("[SourceDLSSG Ampere] {}", message); })) {
				const auto snapshot = trp::ampere::Snapshot();
				Fail(snapshot.error ? snapshot.error : "Ampere startup preparation failed");
			}
			spdlog::info("[SourceDLSSG Ampere] prepared {} fatbin containers before slInit", trp::ampere::Snapshot().fatbins);
		}
	}
	void MFGUnlock::Prepare(ID3D12Device*, const std::filesystem::path& directory)
	{
		if (started_) { return; }
		started_ = true;
		if (state_.route == MFGRoute::Unselected) { Fail("MFG adapter was not selected before Streamline initialization"); }
		if (state_.UsesAmpereUnlock()) {
			wrapper_ = RetainConfiguredModule(directory, L"sl.dlss_g.dll");
			provider_ = RetainConfiguredModule(directory, L"nvngx_dlssg.dll");
			if (!wrapper_ || !provider_) { Fail("prepared Ampere modules changed during Streamline initialization"); }
			const auto snapshot = trp::ampere::Snapshot();
			state_.wrapperPatched = state_.providerPatched = state_.temporalReady = snapshot.prepared && snapshot.bridgeInstalled;
			state_.status = "experimental Ampere MFG; provider and temporal program prepared";
			Tick();
			return;
		}
		if (!state_.UsesAdaUnlock()) {
			state_.status = state_.requested ? "native NVIDIA runtime; Ada unlock not applicable" : "native NVIDIA runtime; Ada unlock disabled by configuration";
			spdlog::info("[SourceDLSSG MFG] startup path: {}; runtime capabilities remain authoritative", state_.status);
			return;
		}
		spdlog::info("[SourceDLSSG MFG] startup path: Ada unlock; locating capability and temporal patches");
		state_.status = "locating Ada patch targets";
		wrapper_ = RetainConfiguredModule(directory, L"sl.dlss_g.dll");
		provider_ = RetainConfiguredModule(directory, L"nvngx_dlssg.dll");
		providerPath_ = directory / L"nvngx_dlssg.dll";
		if (!wrapper_ || !provider_ || !GetProcAddress(wrapper_, "slGetPluginFunction") ||
			!dlssg_provider_policy::IsDlssgImplementationModule(provider_) ||
			!GetProcAddress(provider_, "NVSDK_NGX_GetGPUArchitecture") ||
			!GetProcAddress(provider_, "NVSDK_NGX_D3D12_CreateFeature")) { Fail("configured DLSS-G module or required export missing; no patches applied"); return; }
		auto wrapper = MFGPatch::FindExecutable(wrapper_, MFGContract::wrapperPattern.size(), MFGContract::MatchesWrapper);
		auto provider = MFGPatch::FindExecutable(provider_, MFGContract::providerPattern.size(), MFGContract::MatchesProvider);
		if (!wrapper || !provider || !MFGPatch::ProviderBranchMatches(provider_, provider)) { Fail("capability patch targets are missing, ambiguous or have an unsupported branch layout; no patches applied"); return; }
		constexpr std::array<std::uint8_t, 3> nops3{ 0x90, 0x90, 0x90 };
		const auto wrapperOriginal = std::span(MFGContract::wrapperPattern).subspan(7, 3);
		state_.wrapperPatched = MFGPatch::WriteCode(wrapper + 7, wrapperOriginal, nops3, state_.unsafeMemory);
		if (!state_.wrapperPatched) { Fail("wrapper patch failed"); return; }
		state_.providerPatched = MFGPatch::WriteCode(provider + 2, MFGContract::providerOriginal, MFGContract::providerReplacement, state_.unsafeMemory);
		if (!state_.providerPatched) {
			MFGPatch::WriteCode(wrapper + 7, nops3, wrapperOriginal, state_.unsafeMemory);
			state_.wrapperPatched = false; Fail("provider patch failed; MFG held off"); return;
		}
		spdlog::info("[SourceDLSSG MFG] capability patches wrapperRVA=0x{:X} providerRVA=0x{:X}; host ceiling=x6 after full readiness",
			wrapper + 7 - reinterpret_cast<std::uint8_t*>(wrapper_), provider + 2 - reinterpret_cast<std::uint8_t*>(provider_));
		Tick();
	}
	void MFGUnlock::BindWrapper(const void* setOptions)
	{
		if (!state_.UsesCompatibilityUnlock() || state_.failed) { return; }
		MEMORY_BASIC_INFORMATION memory{};
		state_.wrapperBound = setOptions && VirtualQuery(setOptions, &memory, sizeof(memory)) == sizeof(memory) && memory.AllocationBase == wrapper_;
		if (!state_.wrapperBound) { Fail("slDLSSGSetOptions does not belong to patched wrapper"); return; }
		Tick();
	}
	void MFGUnlock::Tick()
	{
		if (state_.UsesAmpereUnlock()) {
			const auto snapshot = trp::ampere::Snapshot();
			if (!trp::ampere::Verify() || snapshot.failed) { Fail(snapshot.error ? snapshot.error : "Ampere publication verification failed"); }
			if (state_.wrapperBound && (!snapshot.requirementsCalls || !snapshot.capabilityCalls || !(snapshot.resolverMask & 8))) {
				Fail("Streamline bypassed the required Ampere NGX startup functions");
			}
			return;
		}
		if (!state_.UsesAdaUnlock() || state_.failed || !state_.providerPatched) { return; }
		if (state_.temporalReady) {
			// Cheap pointer/protection verification, no rescans or allocations once ready.
			if (!midpoint_fix::PatchProvider(provider_, providerPath_.c_str())) {
				state_.temporalReady = false; state_.unsafeMemory = true;
				state_.temporalFailure = midpoint_fix::FailureCode();
				Fail("temporal publication changed; restart required");
			}
			return;
		}
		// The provider may finish filling its descriptor during device/feature init.
		// Try the early lifecycle boundaries, then at most every 60 real Presents.
		if (++ticks_ > 3 && ticks_ % 60 != 0) { return; }
		++state_.attempts;
		state_.temporalReady = midpoint_fix::PatchProvider(provider_, providerPath_.c_str());
		state_.temporalFailure = midpoint_fix::FailureCode();
		state_.unsafeMemory |= MFGContract::PublicationUncertain(state_.temporalFailure);
		if (state_.unsafeMemory) { Fail("temporal publication is unsafe; restart required"); }
		if (state_.temporalReady) {
			state_.status = "temporal patch ready; x2 through x6 and runtime-reported dynamic mode gated by wrapper binding";
			spdlog::info("[SourceDLSSG MFG] temporalReady=true wrapperBound={} attempts={}", state_.wrapperBound, state_.attempts);
		} else if (state_.temporalFailure != 12 || state_.attempts >= 8) {
			Fail("Ada temporal program is unsupported or could not be prepared; see temporal error code");
		} else { state_.status = "waiting for temporal descriptor; x2 fixed only"; }
	}
}
