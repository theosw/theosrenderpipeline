#include <PCH.h>
#include "SourceDLSSGBackend.h"
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
	void Backend::ConfigureNeuralRendering(NeuralOptions a_options)
	{
		const bool requested = a_options.enabled;
		a_options = SanitizeNeuralOptions(std::move(a_options));
		std::scoped_lock lock(neuralMutex_);
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
	}
	const std::string* Backend::SelectNeuralRuntime(NeuralOptions& options)
	{
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
	const char* Backend::NeuralUnavailableReason(const NeuralOptions& options)
	{
		if (neuralRecovery_.Disabled()) { return "NR disabled for this session"; }
		if (startupTransport_) { return "NR unavailable after frame-generation startup failure; restart to retry"; }
		std::scoped_lock lock(neuralMutex_);
		if (!NvidiaAdapter() && options.enabled) { return "Neural Rendering requires an NVIDIA RTX GPU; XeSS and XeFG remain available"; }
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
    bool Backend::RecreateNeuralIfNeeded(const NeuralOptions& options)
    {
        if (!neuralPass_ || !neuralPass_->NeedsRecreation(options, motion_.desc.Width, motion_.desc.Height)) { return true; }
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
        if (!neuralPass_->NeedsResourceRecreation(options, motion_.desc.Width, motion_.desc.Height)) {
            if (!neuralPass_->RetireChangedModels(options)) { return FailNeuralRecording(); }
            logger::info("[SourceDLSSG NR] model change: GPU drain {:.0f} ms, selective retirement {:.0f} ms; resources and unaffected features retained",
                drained, since(releasing));
            return true;
        }
        if (!neuralPass_->ReleaseAfterRetirement()) { return FailNeuralRecording(); }
        neuralPass_.reset();
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
    void Backend::ApplyNeuralRetry()
    {
        if (!neuralRetryRequested_.exchange(false, std::memory_order_acq_rel)) { return; }
        if (!neuralRecovery_.Retryable()) { return; }
        // Recovery already retired and destroyed the previous owner. No vendor
        // call or list is active here. Retained/unsafe owners never reach this.
        if (!NeuralRendering::ResetLaunchHookAfterRetirement()) { return; }
        neuralRecovery_.Retry();
        neuralHistory_.Invalidate();
        std::scoped_lock lock(neuralMutex_);
        neuralAvailability_.Reset();
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
                return (!neuralPass_ || neuralPass_->RecoverySafe()) && NeuralRendering::LaunchHookRecoverySafe();
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
        frameNeuralReset_ = true;
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

    bool Backend::EvaluateNeuralBeforeUpscaling(const NeuralOptions& options, const sl::Constants* camera,
        bool eligible, ID3D11Texture2D* color, ID3D11Texture2D* motion, ID3D11Texture2D* depth,
        FrameExtent renderExtent, bool& reset)
    {
        return EvaluateNeuralWorld(options, camera, eligible, color, motion, depth, renderExtent, renderExtent, reset);
    }

    bool Backend::EvaluateNeuralWorld(const NeuralOptions& options, const sl::Constants* camera,
        bool eligible, ID3D11Texture2D* color, ID3D11Texture2D* motion, ID3D11Texture2D* depth,
        FrameExtent renderExtent, FrameExtent colorExtent, bool& reset)
    {
        if (!Ready()) { return false; }
        ApplyNeuralRetry();
        frameNeuralOptions_ = options;
        // Preserve the saved after-DLSS preference while enforcing a world-only
        // input contract for the early stage. There are no UI pixels to correct.
        if (frameNeuralOptions_.WorldOnly()) { frameNeuralOptions_.tuning.uiCorrection = false; }
        neuralFrameBegun_ = true;
        neuralEvaluatedEarly_ = false;
        neuralEligible_ = eligible && !TransitionBlocked() && (!options.WorldOnly() || camera) &&
            !NeuralUnavailableReason(frameNeuralOptions_);
        frameNeuralReset_ = neuralHistory_.ResetFor(frameNeuralOptions_, neuralEligible_,
            reset || (camera && camera->reset == sl::eTrue));
        reset |= frameNeuralReset_;
        // The upscaler still needs to discard a history fed by a changed NR
        // model. Keep this separate from the NR chain's per-pass reset mask.
        frameNeuralModelChanged_ = neuralPass_ && neuralPass_->ModelChangePending(frameNeuralOptions_);
        reset |= options.beforeUpscaling && frameNeuralModelChanged_;
        if (!options.enabled || !options.WorldOnly() || !neuralEligible_) { return true; }

        auto* timing = PerformanceTuning::GetSingleton();
        const bool measure = timing->TimingEnabled();
        const auto cpuBegin = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // One D3D11 clock domain spans input copies, the D3D12 dependency and
        // the return copy. This is elapsed GPU time, including scheduling gaps.
        ScopedD3D11PerformanceStage gpuTimer{context11_.Get(), PerformanceTuning::D3D11Stage::kNeuralEarlyRoundTrip};
        AllocatorWaitTiming allocatorWait{};
        if (!renderExtent.width || !renderExtent.height ||
            !EnsureGuide(motion, motion_, DXGI_FORMAT_UNKNOWN, renderExtent, true) ||
            !EnsureGuide(depth, depth_, DXGI_FORMAT_R32_FLOAT, renderExtent, true) ||
            !EnsureGuide(color, earlyNeuralColor_, DXGI_FORMAT_UNKNOWN, colorExtent, true)) {
            reset = true;
            return FAILED(fault_) ? false : FailNeuralRecording("early NR shared-input allocation refused");
        }
        if (!RecreateNeuralIfNeeded(frameNeuralOptions_)) { return false; }
        if (neuralRecovery_.Disabled()) { reset = true; return true; }
        if (!CopyObjectMask(renderExtent) || !CopyDepth(depth) || !Check(interop_.CopyInputRegion(motion, motion_, renderExtent), "early NR motion copy") ||
            !Check(interop_.CopyInputRegion(color, earlyNeuralColor_, colorExtent), "early NR world copy") ||
            !Check(interop_.SignalD3D11(Work::Upscaling), "early NR inputs ready")) { return false; }
        ID3D12GraphicsCommandList* list = nullptr;
        if (!Check(interop_.Begin(Work::Upscaling, &list, measure ? &allocatorWait : nullptr), "begin NR before DLSS")) { return false; }
        bool recorded{};
        try {
        if (!neuralPass_) { neuralPass_ = std::make_unique<NeuralPass>(); }
        recorded = neuralPass_->Record(device12_.Get(), list, interop_.CurrentSlot(Work::Upscaling), frameNeuralOptions_,
            frameNeuralReset_, camera->depthInverted == sl::eTrue,
            camera->mvecScale.x * motion_.desc.Width, camera->mvecScale.y * motion_.desc.Height,
            motion_.texture12.Get(), depth_.texture12.Get(), nullptr, earlyNeuralColor_.texture12.Get(), nullptr,
            neuralTimestampFrequency_, interop_.Marks(Work::Upscaling), objectMaskCurrent_ ? objectMask_.texture12.Get() : nullptr,
            frameNeuralOptions_.beforeUpscaling ? std::array<float,2>{} :
                std::array<float,2>{camera->jitterOffset.x,camera->jitterOffset.y},
            frameNeuralOptions_.beforeUpscaling ? std::array<float,2>{camera->jitterOffset.x,camera->jitterOffset.y} :
                std::array<float,2>{});
        } catch (const std::bad_alloc&) { reset = true; return FailNeuralRecording("NR CPU allocation failed"); }
        if (!recorded) {
            // Producer colour was never overwritten; discard every transition
            // and inference command, then let reconstruction consume that colour.
            const bool recovered = FailNeuralRecording();
            reset = true;
            return recovered;
        }
        if (!Check(Interop::RecordCopy(list, neuralPass_->Corrected(), earlyNeuralColor_.texture12.Get()), "early NR result copy") ||
            !Check(interop_.Submit(Work::Upscaling), "submit NR before DLSS") ||
            !Check(interop_.WaitD3D11(Work::Upscaling), "DLSS waits for NR")) { return false; }
        // This copy and the subsequent NGX D3D11 call follow the GPU wait on
        // the same immediate context. No CPU-wide flush/drain on each frame.
        if (!Check(D3D11FrameCopy::Color(context11_.Get(), earlyNeuralColor_.texture11.Get(), color, colorExtent),
            "early NR return copy")) { return false; }
        neuralEvaluatedEarly_ = true;
        if (measure) {
            timing->RecordNeuralEarlyCPU(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - cpuBegin).count()), allocatorWait.nanoseconds, allocatorWait.waited);
        }
        return true;
    }
}
