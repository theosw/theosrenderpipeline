#include <PCH.h>
#include "SourceDLSSGMFG.h"
#include "../PluginPaths.h"
#include "CommunityShaderIntegration.h"
#include "../StartupHints.h"
#include "../SessionDegradationLog.h"
#include "DlssgOverride.h"
#include "NvidiaDriverVersion.h"
#include "MFGStartupPolicy.h"
#include "../../extern/dlssg-turing/src/SourceDLSSGMFGPatch.h"
#include "../../extern/dlssg-turing/extern/RTX40MFG/midpoint_fix.h"
#include "../../extern/dlssg-turing/extern/RTX40MFG/dlssg_provider_policy.h"
#include "../../extern/dlssg-turing/src/runtime.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstring>
#include <format>
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
		Fail(reason, (state_.adapter == midpoint_fix::AdapterKind::Ampere || state_.adapter == midpoint_fix::AdapterKind::Turing) ?
			std::string("RTX 20/30-series frame generation requires the Universal compatibility path.\n"
				"Keep SourceDLSSGMFGUnlock=true and include TheosRenderPipeline.log when reporting this startup failure.\n") :
			std::format("Temporal error: {}. Attempts: {}.\n\n"
				"Set SourceDLSSGMFGUnlock=false under [Experimental] in SKSE/Plugins/TheosRenderPipeline.User.ini to use the unmodified NVIDIA runtime.\n",
				state_.temporalFailure, state_.attempts));
	}
	[[noreturn]] void MFGUnlock::Fail(const char* reason, const std::string& guidance)
	{
		state_.failed = true; state_.status = reason;
		spdlog::error("[SourceDLSSG MFG] startup/verification failed: {}", reason);
		const auto hint = StartupHints::HintFor(reason, StartupHints::CurrentFacts(reason));
		if (!hint.empty()) { spdlog::info("[SourceDLSSG MFG] {}", hint); }
		util::report_and_fail(std::format(
			"Theo's Render Pipeline: MFG startup or patch verification failed.\n\n"
			"{}\n\n{}{}{}"
			"See TheosRenderPipeline.log for details. Skyrim will close after this message.", reason, guidance, hint, hint.empty() ? "" : "\n"));
	}
	bool MFGUnlock::Refuse(std::string reason)
	{
		state_.failed = true;
		refusal_ = std::move(reason);
		state_.status = refusal_.c_str();
		spdlog::warn("[SourceDLSSG MFG] frame generation cannot start: {}; nothing was patched, continuing without it", refusal_);
		return false;
	}
	void MFGUnlock::StandDown(const char* reason)
	{
		if (!state_.UsesAdaUnlock() || state_.unsafeMemory) { Fail(reason); }
		state_.failed = true;
		standDownStatus_ = std::format("MFG unavailable this session: {}. x2 frame generation continues.{}", reason,
			nvidiaOverride_ ? " NVIDIA app is overriding DLSS Frame Generation for Skyrim; set its Frame Generation overrides to "
				"Use the 3D application setting to restore MFG." : "");
		state_.status = standDownStatus_.c_str();
		spdlog::warn("[SourceDLSSG MFG] {}", standDownStatus_);
		Degradation::Note(Degradation::Reason::MultiFrameGeneration, standDownStatus_);
	}
	bool MFGUnlock::CheckBackportDriver(ID3D12Device* device)
	{
		Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
		Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
		LARGE_INTEGER userMode{};
		const bool read = device && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
			SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
			SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &userMode));
		const auto version = read ? NvidiaDriverVersion(userMode.LowPart) : 0u;
		if (!version) {
			spdlog::warn("[SourceDLSSG MFG] NVIDIA driver version unavailable; RTX 20/30 frame generation needs {} or newer",
				DriverVersionText(kBackportRecommendedDriver));
			return true;
		}
		spdlog::info("[SourceDLSSG MFG] NVIDIA driver {}; RTX 20/30 frame generation needs {} or newer",
			DriverVersionText(version), DriverVersionText(kBackportMinimumDriver));
		if (BackportDriverSupported(version)) { return true; }
		// Known from the version alone, before any module is loaded: the kernels would fail to compile every frame.
		return Refuse(std::format("NVIDIA driver {} is too old for RTX 20/30 frame generation", DriverVersionText(version)));
	}
	void MFGUnlock::EnterStartupScope() noexcept { trp::ampere::EnterStartupScope(); }
	void MFGUnlock::LeaveStartupScope() noexcept { trp::ampere::LeaveStartupScope(); }
	bool MFGUnlock::SelectAdapter(ID3D12Device* device)
	{
		if (state_.route != MFGRoute::Unselected) { Fail("MFG startup adapter selection was repeated"); }
		state_.status = "selecting NVIDIA MFG path";
		spdlog::info("[SourceDLSSG MFG] stage=adapter-selection compatibilityRequested={}", state_.requested);
		midpoint_fix::SetLogCallback([](const wchar_t* text) {
			// Upstream callback is noexcept; diagnostics must not terminate the game.
			try { spdlog::info("[SourceDLSSG MFG] {}", std::filesystem::path(text).string()); } catch (...) {}
		});
		auto adapter = midpoint_fix::ObserveD3D12Adapter(device);
		if (emulateRTX30_ && adapter == midpoint_fix::AdapterKind::Ada) {
			spdlog::warn("[SourceDLSSG MFG] DEVELOPER TEST: EmulateRTX30OnRTX40 runs the RTX 30 (SM86) path on this RTX 40 adapter");
			trp::ampere::EmulateAmpereOnAda(true);
			adapter = midpoint_fix::AdapterKind::Ampere;
		}
		state_.SelectRoute(adapter);
		if (state_.requested && adapter == midpoint_fix::AdapterKind::Unavailable) {
			state_.temporalFailure = midpoint_fix::FailureCode();
			const auto reason = std::format("the rendering GPU could not be identified (code {}); no patches applied, "
				"NVIDIA's own frame generation runs where the GPU supports it", state_.temporalFailure);
			spdlog::warn("[SourceDLSSG MFG] {}", reason);
			Degradation::Note(Degradation::Reason::AdapterUnidentified, reason);
		}
		spdlog::info("[SourceDLSSG MFG] selected route={} compatibilityRequested={}",
			state_.UsesTuringUnlock() ? "Turing" : state_.UsesAmpereUnlock() ? "Ampere" : state_.UsesAdaUnlock() ? "Ada" : "Native", state_.requested);
		if ((adapter == midpoint_fix::AdapterKind::Ampere || adapter == midpoint_fix::AdapterKind::Turing) && !state_.requested) {
			return Refuse("RTX 20/30-series detected, but SourceDLSSGMFGUnlock=false disables its compatibility path. "
				"Set it to true in the winning MO2 INI (including Overwrite) and restart");
		}
		return !state_.UsesProviderBackport() || CheckBackportDriver(device);
	}
	bool MFGUnlock::BeforeStreamline(ID3D12Device* device, const std::filesystem::path& directory)
	{
		if (state_.route == MFGRoute::Unselected) { Fail("MFG adapter was not selected before Streamline loading"); }
		if (state_.UsesProviderBackport()) {
			spdlog::info("[SourceDLSSG Ampere] separate runtime bundles permitted={}", CommunityShaders::Active());
            if (!trp::ampere::Start(device, directory, [](const char* message) { spdlog::info("[SourceDLSSG Ampere] {}", message); }, CommunityShaders::Active(),
                [](const char* reason) {
                    spdlog::critical("[SourceDLSSG Turing] {}", reason);
                    spdlog::default_logger()->flush();
                    util::report_and_fail(std::format("Theo's Render Pipeline: RTX20 test stopped.\n\n{}\n\n"
                        "Send TheosRenderPipeline.log to the developer. Skyrim will close after this message.",reason));
                })) {
				const auto snapshot = trp::ampere::Snapshot();
				const char* error = snapshot.error ? snapshot.error : "Ampere startup preparation failed";
				// Refused before anything was published: only frame generation goes.
				// A rollback or a later failure keeps the fatal dialog.
				if (!CanContinueWithoutFrameGeneration(MFGStartupRefusal::PreparationFailed, snapshot.prepared || snapshot.bridgeInstalled, error)) { Fail(error); }
				return Refuse(error);
			}
			spdlog::info("[SourceDLSSG Ampere] prepared {} fatbin containers before slInit", trp::ampere::Snapshot().fatbins);
		}
		return true;
	}
	void MFGUnlock::Prepare(ID3D12Device*, const std::filesystem::path& directory)
	{
		if (started_) { return; }
		started_ = true;
		if (state_.route == MFGRoute::Unselected) { Fail("MFG adapter was not selected before Streamline initialization"); }
		if (state_.UsesProviderBackport()) {
			wrapper_ = RetainConfiguredModule(directory, L"sl.dlss_g.dll");
			provider_ = RetainConfiguredModule(directory, L"nvngx_dlssg.dll");
			if (!wrapper_ || !provider_) { Fail("prepared Ampere modules changed during Streamline initialization"); }
			const auto snapshot = trp::ampere::Snapshot();
			state_.wrapperPatched = state_.providerPatched = state_.temporalReady = snapshot.prepared && snapshot.bridgeInstalled;
			state_.status = state_.UsesTuringUnlock() ? "experimental Turing MFG; SM75 provider and temporal program prepared" : "experimental Ampere MFG; provider and temporal program prepared";
			Tick();
			return;
		}
		if (!state_.UsesAdaUnlock()) {
			state_.status = state_.requested ? "native NVIDIA runtime; compatibility not applicable" : "native NVIDIA runtime; compatibility disabled by configuration";
			spdlog::info("[SourceDLSSG MFG] startup path: {}; runtime capabilities remain authoritative", state_.status);
			return;
		}
		spdlog::info("[SourceDLSSG MFG] startup path: Ada unlock; locating capability and temporal patches");
		state_.status = "locating Ada patch targets";
		wrapper_ = RetainConfiguredModule(directory, L"sl.dlss_g.dll");
		provider_ = RetainConfiguredModule(directory, L"nvngx_dlssg.dll");
		providerPath_ = directory / L"nvngx_dlssg.dll";
		if (!provider_ && (provider_ = RetainNvidiaOverrideDlssg())) {
			// NVIDIA app's override replaced TRP's DLSS-G; try the unlock on that build.
			nvidiaOverride_ = true;
			providerPath_ = PluginPaths::ModulePath(provider_);
			spdlog::warn("[SourceDLSSG MFG] NVIDIA app frame generation override active: DLSS-G loaded from {}", providerPath_.string());
		}
		if (!wrapper_ || !provider_ || !GetProcAddress(wrapper_, "slGetPluginFunction") ||
			!dlssg_provider_policy::IsDlssgImplementationModule(provider_) ||
			!GetProcAddress(provider_, "NVSDK_NGX_GetGPUArchitecture") ||
			!GetProcAddress(provider_, "NVSDK_NGX_D3D12_CreateFeature")) { StandDown("the DLSS-G module or a required export is missing; no patches applied"); return; }
		auto wrapper = MFGPatch::FindExecutable(wrapper_, MFGContract::wrapperPattern.size(), MFGContract::MatchesWrapper);
		auto provider = MFGPatch::FindExecutable(provider_, MFGContract::providerPattern.size(), MFGContract::MatchesProvider);
		if (!wrapper || !provider || !MFGPatch::ProviderBranchMatches(provider_, provider)) {
			StandDown("this DLSS-G build's capability patch targets are missing, ambiguous or have an unsupported layout; no patches applied"); return;
		}
		constexpr std::array<std::uint8_t, 3> nops3{ 0x90, 0x90, 0x90 };
		const auto wrapperOriginal = std::span(MFGContract::wrapperPattern).subspan(7, 3);
		state_.wrapperPatched = MFGPatch::WriteCode(wrapper + 7, wrapperOriginal, nops3, state_.unsafeMemory);
		if (!state_.wrapperPatched) { StandDown("wrapper patch failed"); return; }
		state_.providerPatched = MFGPatch::WriteCode(provider + 2, MFGContract::providerOriginal, MFGContract::providerReplacement, state_.unsafeMemory);
		if (!state_.providerPatched) {
			MFGPatch::WriteCode(wrapper + 7, nops3, wrapperOriginal, state_.unsafeMemory);
			state_.wrapperPatched = false; StandDown("provider patch failed; wrapper patch reverted"); return;
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
		if (!state_.wrapperBound) {
			if (state_.UsesAdaUnlock()) { StandDown("slDLSSGSetOptions does not belong to the patched wrapper"); return; }
			Fail("slDLSSGSetOptions does not belong to patched wrapper");
		}
		Tick();
	}
	void MFGUnlock::Tick()
	{
		if (state_.UsesProviderBackport()) {
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
			StandDown("the Ada temporal program is unsupported or could not be prepared (see temporal error code)");
		} else { state_.status = "waiting for temporal descriptor; x2 fixed only"; }
	}
}
