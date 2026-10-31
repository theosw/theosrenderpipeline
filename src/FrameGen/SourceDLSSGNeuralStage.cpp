#include <PCH.h>
#include "SourceDLSSGBackend.h"
#include "NeuralRenderingRuntimeIdentity.h"
#include "PerformanceTuning.h"
#include "NeuralRuntimeCompatibility.h"
#include "NeuralRuntimeKernels.h"
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
        neuralAdmissionChecked_ = false;
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
	bool Backend::RefuseNeuralCreation(const std::filesystem::path& runtime)
	{
		const auto result = neuralPass_ ? neuralPass_->RefusedCreation() : 0u;
		if (!result) { return false; }
		const auto status = neuralPass_->Status();
		// Only Pass 1's creation reports a refusal, and it stops before Record
		// writes to the open list. Earlier frames of this pass may still be running
		// in other command slots, so retire them before releasing its resources.
		// The open list is unsubmitted and Drain waits only for submitted work.
		if (!Check(interop_.Drain(), "retire NR before dropping a refused runtime")) { return false; }
		neuralPass_.reset();
		std::scoped_lock lock(neuralMutex_);
		RefuseNeuralRuntime(runtime, std::format("could not start NR ({})", status), "");
		return true;
	}
	const char* Backend::NeuralUnavailableReason(const NeuralOptions& options, FrameExtent sceneExtent)
	{
		if (startupTransport_) { return "NR unavailable after frame-generation startup failure; restart to retry"; }
		std::scoped_lock lock(neuralMutex_);
		if (!NvidiaAdapter() && options.enabled) { return "Neural Rendering requires an NVIDIA RTX GPU; XeSS and XeFG remain available"; }
        if (options.enabled && neuralCapability_.Pascal() && !NeuralRendering::PascalResearchFor(neuralComputeCapability_)) {
            // Early admission precedes creation of motion_ and the NR guides.
            // Use the caller's real scene extent, then re-admit after dimension/pass changes.
            const auto width = sceneExtent.width ? sceneExtent.width : (options.beforeUpscaling ? motion_.desc.Width : presenterDesc_.BufferDesc.Width);
            const auto height = sceneExtent.height ? sceneExtent.height : (options.beforeUpscaling ? motion_.desc.Height : presenterDesc_.BufferDesc.Height);
            if (!neuralAdmissionChecked_ || neuralCapability_.estimateBytes != NeuralRendering::EstimatePascalMemory(width, height, options.passes)) {
                neuralCapability_ = NeuralRendering::QueryNeuralAdmission(device12_->GetAdapterLuid(), options.runtimePath,
                    width, height, options.passes, NeuralRendering::RuntimeKernelsSupported(), true);
                if (neuralPass_ && !neuralPass_->NeedsRecreation(options, motion_.desc.Width, motion_.desc.Height)) {
                    neuralCapability_.retainedEstimateBytes = neuralCapability_.estimateBytes;
                }
                neuralAdmissionReason_ = neuralCapability_.Refusal() ? neuralCapability_.Refusal() : "";
                neuralAdmissionChecked_ = width && height;
            }
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
    NeuralRendering::NeuralCapability Backend::CheckNeuralAdmission(const NeuralOptions& options, unsigned width, unsigned height, bool log)
    {
        std::scoped_lock lock(neuralMutex_);
        if (!device12_) { return {}; }
        auto result = NeuralRendering::QueryNeuralAdmission(device12_->GetAdapterLuid(), options.runtimePath,
            width, height, options.passes, NeuralRendering::RuntimeKernelsSupported(), log);
        if (neuralPass_ && !neuralPass_->NeedsRecreation(options, motion_.desc.Width, motion_.desc.Height)) {
            result.retainedEstimateBytes = result.estimateBytes;
        }
        return result;
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
            if (!Check(neuralPass_->RetireChangedModels(options) ? S_OK : E_FAIL,
                "NR model creation retirement unconfirmed; relaunch required")) { return false; }
            logger::info("[SourceDLSSG NR] model change: GPU drain {:.0f} ms, selective retirement {:.0f} ms; resources and unaffected features retained",
                drained, since(releasing));
            return true;
        }
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

    bool Backend::FailNeuralRecording()
    {
        // RecordEvaluation checks hook health before this path. Never execute
        // any part of a list whose inference chain was rejected.
        const auto early = interop_.Discard(Work::Upscaling);
        const auto late = interop_.Discard(Work::SwapChain);
        if (FAILED(early) || FAILED(late)) { return Check(FAILED(early) ? early : late, "discard rejected NR recording"); }
        std::scoped_lock lock(neuralMutex_);
        neuralSnapshot_.failed = true;
        neuralSnapshot_.active = false;
        neuralSnapshot_.status = neuralPass_->Status();
        logger::error("[SourceDLSSG NR] stopped before frame submission: {}", neuralSnapshot_.status);
        // The frame list is unsubmitted; separate feature-creation work may
        // already be queued. Its session retains that context on failed waits.
        return Check(E_FAIL, "source NR pass; relaunch required");
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
        frameNeuralOptions_ = options;
        // Preserve the saved after-DLSS preference while enforcing a world-only
        // input contract for the early stage. There are no UI pixels to correct.
        if (frameNeuralOptions_.WorldOnly()) { frameNeuralOptions_.tuning.uiCorrection = false; }
        neuralFrameBegun_ = true;
        neuralEvaluatedEarly_ = false;
        neuralEligible_ = eligible && !TransitionBlocked() && (!options.WorldOnly() || camera) &&
            !NeuralUnavailableReason(frameNeuralOptions_, options.WorldOnly() ? colorExtent :
                FrameExtent{presenterDesc_.BufferDesc.Width, presenterDesc_.BufferDesc.Height});
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
            !EnsureGuide(motion, motion_, DXGI_FORMAT_UNKNOWN, renderExtent) ||
            !EnsureGuide(depth, depth_, DXGI_FORMAT_R32_FLOAT, renderExtent) ||
            !EnsureGuide(color, earlyNeuralColor_, DXGI_FORMAT_UNKNOWN, colorExtent) ||
            !RecreateNeuralIfNeeded(frameNeuralOptions_)) { return false; }
        if (!CopyObjectMask(renderExtent) || !CopyDepth(depth) || !Check(interop_.CopyInputRegion(motion, motion_, renderExtent), "early NR motion copy") ||
            !Check(interop_.CopyInputRegion(color, earlyNeuralColor_, colorExtent), "early NR world copy") ||
            !Check(interop_.SignalD3D11(Work::Upscaling), "early NR inputs ready")) { return false; }
        ID3D12GraphicsCommandList* list = nullptr;
        if (!Check(interop_.Begin(Work::Upscaling, &list, measure ? &allocatorWait : nullptr), "begin NR before DLSS")) { return false; }
        if (!neuralPass_) { neuralPass_ = std::make_unique<NeuralPass>(); }
        if (!neuralPass_->Record(device12_.Get(), list, interop_.CurrentSlot(Work::Upscaling), frameNeuralOptions_,
            frameNeuralReset_, camera->depthInverted == sl::eTrue,
            camera->mvecScale.x * motion_.desc.Width, camera->mvecScale.y * motion_.desc.Height,
            motion_.texture12.Get(), depth_.texture12.Get(), nullptr, earlyNeuralColor_.texture12.Get(), nullptr,
            neuralTimestampFrequency_, interop_.Marks(Work::Upscaling), objectMaskCurrent_ ? objectMask_.texture12.Get() : nullptr,
            frameNeuralOptions_.beforeUpscaling ? std::array<float,2>{} :
                std::array<float,2>{camera->jitterOffset.x,camera->jitterOffset.y},
            frameNeuralOptions_.beforeUpscaling ? std::array<float,2>{camera->jitterOffset.x,camera->jitterOffset.y} :
                std::array<float,2>{})) {
            if (!RefuseNeuralCreation(frameNeuralOptions_.runtimePath)) { return FailNeuralRecording(); }
            // Nothing was recorded: retire the empty list and run DLSS on the original
            // colour this frame. The next frame uses the next runtime.
            neuralEligible_ = false;
            return Check(interop_.Submit(Work::Upscaling), "submit NR list after a refused runtime");
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
