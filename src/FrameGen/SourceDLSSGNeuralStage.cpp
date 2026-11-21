#include <PCH.h>
#include "NvidiaHost.h"
#include "SourceDLSSGBackend.h"
#include "VulkanNRHost.h"
#include "NeuralRuntimeSession.h"
#include "NeuralRenderingRuntimeIdentity.h"
#include "PerformanceTuning.h"
#include "NeuralRuntimeCompatibility.h"
#include "NeuralRuntimeKernels.h"
#include "NeuralLaunchHook.h"
#include "SourceDLSSGSettings.h"
#include "../PluginPaths.h"
#include "../NeuralObjectHooks.h"
#include <algorithm>
#include <chrono>
#include <format>

namespace TheosRenderPipeline::SourceDLSSG
{
	void Backend::ConfigureNeuralRendering(NeuralOptions a_options, const char* origin)
	{
		const bool requested = a_options.enabled;
		a_options = SanitizeNeuralOptions(VendorNeutralOptions(std::move(a_options)));
		std::scoped_lock lock(neuralMutex_);
        neuralAdmissionCache_.Reset();
        neuralAdmissionReason_.clear();
		// The note stays only while the selected runtime is the one being replaced.
		const auto* note = SelectNeuralRuntime(a_options);
		neuralSnapshot_.fallback = note ? *note : std::string{};
		if (requested && !a_options.enabled) {
			logger::warn("[SourceDLSSG NR] optional runtime not found at {}; NR disabled, standard DLSS continues",
				a_options.runtimePath.string());
		}

		if (a_options.runtimePath != neuralOptions_.runtimePath || (!neuralOptions_.enabled && a_options.enabled)) {
			neuralAvailability_.Reset();
		}
		NeuralObjects::Configure(a_options.enabled && NeuralRendering::ObjectsInUse(a_options.objectMasks, a_options.passes));
        neuralOptions_ = std::move(a_options);
        LogNeuralDecision(origin);
	}
    void Backend::LogNeuralDecision(const char* origin)
    {
        const auto caps = NeuralRendering::VulkanNRHost::Probe(device12_.Get()); // observe only
        const auto* hip = device12_ ? AmdNeuralPass::Availability(AmdAdapter()) : "pending rendering device";
        const auto* owner = NeuralRendering::runtimeSession.Refusal(neuralOptions_.backend);
        logger::info("[NR backend] {}: chosen={} reason=configured preference after hardware defaults enabled={} runtime={} runtimeSelection={} fallback={} session={} HIP={} Vulkan={} VulkanPath={} device={}",
            origin, NeuralRendering::BackendName(neuralOptions_.backend), neuralOptions_.enabled,
            neuralOptions_.runtimePath.string(), neuralOptions_.automaticRuntime ? "automatic" : "explicit",
            neuralSnapshot_.fallback, owner ? owner : "admitted", hip ? hip : "available",
            caps.available ? "available (cached)" : caps.reason, caps.pathReason, caps.device);
        logger::info("[NR backend] {}: HIP execution={} inputScale={} XeSSReady={} FSRReady={}",origin,
            neuralOptions_.amdWaitForNetwork ? "Regular (wait)" : "Async",neuralOptions_.reconstruction.inputScale,xess_.Ready(),fsr_.Ready());
    }
	const std::string* Backend::SelectNeuralRuntime(NeuralOptions& options)
	{
        if (options.backend == NeuralRendering::Backend::HIP) { options.runtimePath = SourceDLSSG::AmdNeuralPass::RuntimePath(); return nullptr; }
        if (options.backend == NeuralRendering::Backend::Vulkan) { options.runtimePath = NeuralRendering::VulkanNRHost::Path(); return nullptr; }
		if (!options.enabled) { return nullptr; }
		auto refused = [&](const std::filesystem::path& path) -> const std::string* {
			const auto entry = std::find_if(neuralRefusedRuntimes_.begin(), neuralRefusedRuntimes_.end(),
				[&](const auto& item) { return PluginPaths::EqualPath(item.first, path); });
			return entry == neuralRefusedRuntimes_.end() ? nullptr : &entry->second;
		};
		const std::string* note{};
		if (!NeuralRendering::PascalResearchFor(neuralComputeCapability_) && !options.automaticRuntime && !(note = refused(options.runtimePath))) { return nullptr; }
		// Until the device exists any present candidate can stand in; nothing
		// creates a feature before CreateSwapChain chooses again.
		if (neuralComputeCapability_ < 0 && device12_) {
			neuralComputeCapability_ = NeuralRendering::ComputeCapability(device12_->GetAdapterLuid());
		}
		for (const auto candidate : AutomaticNeuralRuntimes(neuralComputeCapability_, NeuralRendering::RuntimeKernelsSupported())) {
			const auto path = PluginPaths::Directory() / std::filesystem::u8path(candidate);
			if (const auto* why = refused(path)) { note = note ? note : why; continue; }
			std::error_code error;
			if (std::filesystem::is_regular_file(path, error)) {
				options.runtimePath = path;
				if (neuralComputeCapability_ >= 0 && !PluginPaths::EqualPath(path, neuralLoggedRuntime_)) {
					neuralLoggedRuntime_ = path;
					logger::info("[SourceDLSSG NR] {} chose {} for GPU compute capability {}.{}",
						options.automaticRuntime ? "Automatic" : "fallback", path.filename().string(),
						neuralComputeCapability_ / 10, neuralComputeCapability_ % 10);
				}
				return note;
			}
		}
		options.enabled = false;
		return note;
	}
	void Backend::RefuseNeuralRuntime(const std::filesystem::path& runtime, const std::string& reason, std::string_view advice)
	{
		// Both NR stages of one frame can ask before the switch lands; record it once.
		if (std::any_of(neuralRefusedRuntimes_.begin(), neuralRefusedRuntimes_.end(),
			[&](const auto& entry) { return PluginPaths::EqualPath(entry.first, runtime); })) { return; }
		neuralRefusedRuntimes_.emplace_back(runtime, std::string{});
		auto next = neuralOptions_;
		next.runtimePath = runtime;
		SelectNeuralRuntime(next);
		const auto name = runtime.filename().string();
		neuralRefusedRuntimes_.back().second = next.enabled ?
			std::format("{} {}, so {} is used instead.{}", name, reason, NeuralRuntimeLabel(next.runtimePath.string()), advice) :
			std::format("{} {}, so NR is off.{}", name, reason, advice);
		logger::warn("[SourceDLSSG NR] {} (GPU compute capability {}.{})", neuralRefusedRuntimes_.back().second,
			neuralComputeCapability_ / 10, neuralComputeCapability_ % 10);
		if (const auto* note = SelectNeuralRuntime(neuralOptions_)) { neuralSnapshot_.fallback = *note; }
		neuralAvailability_.Reset();
	}
	const char* Backend::NeuralUnavailableReason(const NeuralOptions& options, FrameExtent sceneExtent)
	{
		if (neuralRecovery_.Disabled()) { return "NR disabled for this session"; }
		if (startupTransport_) { return "NR unavailable after frame-generation startup failure; restart to retry"; }
		if (GuidesRefused()) { return guideRefusal_.c_str(); }
		std::scoped_lock lock(neuralMutex_);
        if (NeuralRendering::TestBackend(options.backend) && options.enabled) {
            // On Community Shaders (worldOnly) CS upscales; the world pass runs before it.
            if (!options.worldOnly && !xess_.Ready() && !fsr_.Ready()) { return NeuralRendering::kVendorUpscalerReason; }
            if (options.backend == NeuralRendering::Backend::HIP) { return AmdNeuralPass::Availability(AmdAdapter()) ? AmdNeuralPass::Availability(AmdAdapter()) : AmdNeuralPass::AdmissionReason(options, options.worldOnly || NvidiaHost::GetSingleton()->XeSSActive()); }
            const auto caps = NeuralRendering::VulkanNRHost::Probe(device12_.Get(), false, true);
            neuralAdmissionReason_ = caps.available ? "" : caps.reason;
            return neuralAdmissionReason_.empty() ? nullptr : neuralAdmissionReason_.c_str();
        }
        if (!NvidiaAdapter() && options.enabled) { return NeuralRendering::kNativeGPUReason; }
        if (options.enabled && !neuralCapability_.Pascal()) {
            if (const auto* reason = neuralCapability_.Refusal()) { return reason; }
        }
        if (options.enabled && neuralCapability_.Pascal() && !NeuralRendering::PascalResearchFor(neuralComputeCapability_)) {
            // Early admission precedes creation of motion_ and the NR guides.
            // Use the caller's real scene extent, then re-admit after dimension/pass changes.
            const auto width = sceneExtent.width ? sceneExtent.width : (options.beforeUpscaling ? motion_.desc.Width : presenterDesc_.BufferDesc.Width);
            const auto height = sceneExtent.height ? sceneExtent.height : (options.beforeUpscaling ? motion_.desc.Height : presenterDesc_.BufferDesc.Height);
            const auto retained = neuralPass_ && !neuralPass_->NeedsRecreation(options, motion_.desc.Width, motion_.desc.Height) ?
                NeuralRendering::EstimatePascalMemory(width, height, options.passes) : 0;
            const NeuralRendering::NeuralAdmissionRequest request{options.runtimePath, width, height,
                static_cast<unsigned>(options.passes), options.beforeUpscaling, NeuralRendering::RuntimeKernelsSupported(), retained};
            neuralCapability_ = neuralAdmissionCache_.Get(request, NeuralRendering::NeuralAdmissionCache::Clock::now(), [&] {
                return NeuralRendering::QueryNeuralAdmission(device12_->GetAdapterLuid(), options.runtimePath,
                    width, height, options.passes, NeuralRendering::RuntimeKernelsSupported(), true, retained);
            }, NeuralRendering::QueryNeuralBudget);
            // Pending is not a policy refusal or a runtime failure. Do not enter
            // runtime probing/feature creation until a real world extent exists.
            if (neuralCapability_.extentPending) { return NeuralRendering::kPascalExtentPending; }
            neuralAdmissionReason_ = neuralCapability_.Refusal() ? neuralCapability_.Refusal() : "";
            if (!neuralAdmissionReason_.empty()) { return neuralAdmissionReason_.c_str(); }
        }
		const auto retainedBuild = neuralPass_ ? neuralPass_->RetainedRuntimeBuild(options.runtimePath) :
			NeuralRendering::RuntimeBuild::Unknown;
		if (neuralComputeCapability_ < 0 && device12_) { neuralComputeCapability_ = NeuralRendering::ComputeCapability(device12_->GetAdapterLuid()); }
		const NeuralRendering::IncompatibleRuntime* refused{};
		bool unsupported{};
		const auto reason = neuralAvailability_.UnavailableReason(options, [&](const std::filesystem::path& path) {
			const auto identity = NeuralRenderingRuntimeIdentity::VerifySource(path, true);
			if (identity.matched && (refused = NeuralRendering::FindIncompatibleRuntime(identity.sha256, neuralComputeCapability_))) {
				return NeuralRendering::RuntimeBuild::Unknown;
			}
			const auto build = identity.matched ? NeuralRendering::MatchRuntime(identity.size, identity.sha256, identity.version, true) :
				NeuralRendering::RuntimeBuild::Unknown;
			unsupported = build == NeuralRendering::RuntimeBuild::Unknown;
			return build;
		}, retainedBuild);
		if (refused) {
			// Refuse it before any feature uses it; the next frame runs the next runtime instead.
			RefuseNeuralRuntime(options.runtimePath, std::format("is {}", refused->description), " RTX 40 cards need a fixed build.");
			return "NR runtime refused for this GPU; switching runtime";
		}
		if (unsupported && options.automaticRuntime) {
			// Automatic chose this file only because it exists; try the next candidate.
			RefuseNeuralRuntime(options.runtimePath, "is not a supported NR build", "");
			return "NR runtime unsupported; switching runtime";
		}
		if (reason && reason != neuralReportedUnavailable_) { logger::warn("[SourceDLSSG NR] {}", reason); }
		neuralReportedUnavailable_ = reason;
		return reason;
	}
    NeuralRendering::NeuralCapability Backend::CheckNeuralAdmission(const NeuralOptions& options, unsigned width, unsigned height, bool log,
        bool pollAssets)
    {
        std::scoped_lock lock(neuralMutex_);
        if (!device12_) { return {}; }
        if (NeuralRendering::TestBackend(options.backend)) {
            NeuralRendering::NeuralCapability admission;
            admission.luid = device12_->GetAdapterLuid(); admission.vendorBackend = true;
            if (options.backend == NeuralRendering::Backend::HIP) {
                admission.vendorRefusal = AmdNeuralPass::Availability(AmdAdapter());
                if (!admission.vendorRefusal) admission.vendorRefusal = AmdNeuralPass::AdmissionReason(VendorNeutralOptions(options), xess_.Ready() || options.worldOnly);
            } else {
                const auto caps = NeuralRendering::VulkanNRHost::Probe(device12_.Get(), false, true);
                neuralAdmissionReason_ = caps.available ? "" : caps.reason;
                admission.vendorRefusal = neuralAdmissionReason_.empty() ? nullptr : neuralAdmissionReason_.c_str();
            }
            return admission;
        }
        const bool replacement = neuralPass_ && neuralPass_->NeedsRecreation(options, motion_.desc.Width, motion_.desc.Height);
        const auto retained = neuralPass_ && !replacement ? NeuralRendering::EstimatePascalMemory(width, height, options.passes) : 0;
        auto admission = NeuralRendering::QueryNeuralAdmission(device12_->GetAdapterLuid(), options.runtimePath,
            width, height, options.passes, NeuralRendering::RuntimeKernelsSupported(), log, retained, log, pollAssets);
        admission.budgetPendingRetirement = replacement;
        return admission;
    }
	int Backend::NeuralComputeCapability() const
	{
		std::scoped_lock lock(neuralMutex_); return neuralComputeCapability_;
	}
	NeuralOptions Backend::NeuralConfiguration() const
	{
		std::scoped_lock lock(neuralMutex_);
		auto options = neuralOptions_;
		options.compare = neuralCompare_;
		return options;
	}
	void Backend::ConfigureNeuralCompare(NeuralRendering::CompareSettings a_compare)
	{
		std::scoped_lock lock(neuralMutex_); neuralCompare_ = NeuralRendering::SanitizeCompare(a_compare);
	}
	NeuralRendering::CompareSettings Backend::NeuralCompare() const
	{
		std::scoped_lock lock(neuralMutex_); return neuralCompare_;
	}
	NeuralSnapshot Backend::NeuralState() const
	{
		std::scoped_lock lock(neuralMutex_); return neuralSnapshot_;
	}
    bool Backend::RecreateNeuralIfNeeded(const NeuralOptions& options, FrameExtent guideExtent)
    {
        const auto guideWidth = guideExtent.width ? guideExtent.width : motion_.desc.Width;
        const auto guideHeight = guideExtent.height ? guideExtent.height : motion_.desc.Height;
        if (!neuralPass_ || !neuralPass_->NeedsRecreation(options, guideWidth, guideHeight, earlyNeuralColor_.texture12.Get())) { return true; }
        // Private outputs are copied into stable shared textures. Only our
        // queues read the feature, and no command list may be open here.
        const auto started = std::chrono::steady_clock::now();
        const auto since = [](auto from) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
        };
        if (!Check(interop_.Drain(), "retire NR before placement/reconstruction change")) { return false; }
        const double drained = since(started);
        neuralPass_->RetireTelemetry();
        {
            std::scoped_lock lock(neuralMutex_);
            neuralSnapshot_.telemetry = neuralPass_->Telemetry();
        }
        const auto releasing = std::chrono::steady_clock::now();
        if (!neuralPass_->AMD() && !neuralPass_->NeedsResourceRecreation(options, guideWidth, guideHeight)) {
            if (!neuralPass_->RetireChangedModels(options)) { return FailNeuralRecording(); }
            neuralAdmissionCache_.Reset();
            logger::info("[SourceDLSSG NR] model change: GPU drain {:.0f} ms, selective retirement {:.0f} ms; resources and unaffected features retained",
                drained, since(releasing));
            return true;
        }
        if (!neuralPass_->ReleaseAfterRetirement()) { return FailNeuralRecording(); }
        neuralPass_.reset();
        neuralAdmissionCache_.Reset(); // DXGI usage is re-queried after release
        neuralAdmissionReason_.clear();
        logger::info("[SourceDLSSG NR] rebuild: GPU drain {:.0f} ms, previous feature released in {:.0f} ms",
            drained, since(releasing));
        logger::info("[SourceDLSSG NR] retired feature for placement={} passes={} preset={} inputScale={} resolve={} HDR={} producerColor={} peripheral={} fusion={}",
            options.beforeUpscaling ? "before DLSS" : "after DLSS", options.passes, options.reconstruction.preset,
            options.reconstruction.inputScale, static_cast<unsigned>(options.reconstruction.method), options.reconstruction.colorIsHDR,
            options.reconstruction.producerColor, options.reconstruction.peripheralCompression, options.reconstruction.fusedPreparation);
        for (std::size_t i = 0; i + 1 < static_cast<std::size_t>(options.passes); ++i) {
            const auto extra = options.EffectiveExtra(i);
            logger::info("[SourceDLSSG NR] pass{} linked={} preset={} inputScale={} intensity={}",
                i + 2, options.extraPasses[i].linked, extra.preset, extra.inputScale, extra.tuning.intensity);
        }
        return true;
    }

    void Backend::RequestNeuralRetry()
    {
        neuralRetryRequested_.store(true, std::memory_order_release);
    }
    void Backend::ApplyNeuralRetry(const NeuralOptions& options, FrameExtent sceneExtent)
    {
        if (!neuralRetryRequested_.exchange(false, std::memory_order_acq_rel)) { return; }
        if (!neuralRecovery_.Retryable()) { return; }
        if (!options.enabled) { return; }
        const auto width = sceneExtent.width ? sceneExtent.width : (options.beforeUpscaling ? motion_.desc.Width : presenterDesc_.BufferDesc.Width);
        const auto height = sceneExtent.height ? sceneExtent.height : (options.beforeUpscaling ? motion_.desc.Height : presenterDesc_.BufferDesc.Height);
        const auto admission = CheckNeuralAdmission(options, width, height, true);
        if (admission.extentPending || admission.Refusal()) { return; } // preserve reason and unchanged-Apply affordance
        // Recovery already retired and destroyed the previous owner. No vendor
        // call or list is active here. Retained/unsafe owners never reach this.
        if (!NeuralRendering::TestBackend(options.backend) && !NeuralRendering::ResetLaunchHookAfterRetirement()) { return; }
        neuralRecovery_.Retry();
        neuralHistory_.Invalidate();
        std::scoped_lock lock(neuralMutex_);
        neuralAvailability_.Reset();
        neuralAdmissionCache_.Reset();
        neuralAdmissionReason_.clear();
        neuralReportedUnavailable_ = nullptr;
        neuralSnapshot_.failed = neuralSnapshot_.sessionDisabled = neuralSnapshot_.retryable = false;
        neuralSnapshot_.reason.clear();
        logger::info("[SourceDLSSG NR] Apply: retry through normal NR admission");
    }
    bool Backend::FailNeuralRecording(std::string failure)
    {
        const auto reason = !failure.empty() ? std::move(failure) : neuralPass_ ? neuralPass_->Status() : "NR allocation failed";
        const bool independent = neuralRecovery_.Refuse(reason,
            [&] {
                const auto early = interop_.Discard(Work::Upscaling);
                const auto late = interop_.Discard(Work::SwapChain);
                return Check(FAILED(early) ? early : late, "discard rejected NR recording");
            },
            [&] { return Check(interop_.Drain(), "retire NR after refusal"); },
            [&] {
                const auto removed = device12_->GetDeviceRemovedReason();
                if (FAILED(removed)) { return Check(removed, reason.c_str()); }
                return (!neuralPass_ || neuralPass_->RecoverySafe()) && (NeuralRendering::TestBackend(neuralOptions_.backend) || NeuralRendering::LaunchHookRecoverySafe());
            },
            [&] {
                if (neuralPass_ && !neuralPass_->ReleaseAfterRetirement()) {
                    neuralRecovery_.reason += "; " + neuralPass_->Status();
                    // Retain the WHOLE pass: vendor parameters may reference its
                    // resources. Never call this owner again, including shutdown.
                    (void)neuralPass_.release();
                    return false;
                }
                neuralPass_.reset();
                return true;
            });
        neuralHistory_.Invalidate();
        neuralEligible_ = neuralEvaluatedEarly_ = frameNeuralModelChanged_ = false;
        frameNeuralReset_ = neuralRecoveryReset_ = true;
        {
            std::scoped_lock lock(neuralMutex_);
            neuralSnapshot_.failed = neuralSnapshot_.sessionDisabled = true;
            neuralSnapshot_.active = false;
            neuralSnapshot_.retryable = independent && neuralRecovery_.Retryable();
            neuralSnapshot_.reason = neuralRecovery_.reason + (neuralSnapshot_.retryable ?
                "; NR is off. Apply to retry; saved preference unchanged." : "; NR requires restart.");
            neuralSnapshot_.status = neuralSnapshot_.reason;
            logger::warn("[SourceDLSSG NR] {}", neuralSnapshot_.reason);
        }
        // Unknown submission/creation/patch ownership still faults the backend.
        // A failed release alone is quarantined after queue retirement.
        return independent || Check(E_FAIL, reason.c_str());
    }

#include "SourceDLSSGNeuralWorld.inl"
}
