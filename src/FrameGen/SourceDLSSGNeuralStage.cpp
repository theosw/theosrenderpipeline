#include <PCH.h>
#include "SourceDLSSGBackend.h"
#include "NeuralRenderingRuntimeIdentity.h"
#include "PerformanceTuning.h"
#include <chrono>

namespace TheosRenderPipeline::SourceDLSSG
{
	void Backend::ConfigureNeuralRendering(NeuralOptions a_options)
	{
		const bool requested = a_options.enabled;
		a_options = SanitizeNeuralOptions(std::move(a_options));
		if (requested && !a_options.enabled) {
			logger::warn("[SourceDLSSG NR] optional runtime not found at {}; NR disabled, standard DLSS continues",
				a_options.runtimePath.string());
		}
		std::scoped_lock lock(neuralMutex_);
		if (a_options.runtimePath != neuralOptions_.runtimePath || (!neuralOptions_.enabled && a_options.enabled)) {
			neuralAvailability_.Reset();
		}
		neuralOptions_ = std::move(a_options);
	}
	const char* Backend::NeuralUnavailableReason(const NeuralOptions& options)
	{
		std::scoped_lock lock(neuralMutex_);
		const auto reason = neuralAvailability_.UnavailableReason(options, [](const std::filesystem::path& path) {
			const auto identity = NeuralRenderingRuntimeIdentity::VerifySource(path, true);
			return identity.matched ? NeuralRendering::MatchRuntime(identity.size, identity.sha256, true) :
				NeuralRendering::RuntimeBuild::Unknown;
		});
		if (reason && reason != neuralReportedUnavailable_) { logger::warn("[SourceDLSSG NR] {}", reason); }
		neuralReportedUnavailable_ = reason;
		return reason;
	}
	NeuralOptions Backend::NeuralConfiguration() const
	{
		std::scoped_lock lock(neuralMutex_); return neuralOptions_;
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
        if (!Check(interop_.Drain(), "retire NR before placement/reconstruction change")) { return false; }
        neuralPass_->RetireTelemetry();
        {
            std::scoped_lock lock(neuralMutex_);
            neuralSnapshot_.telemetry = neuralPass_->Telemetry();
        }
        neuralPass_.reset();
        logger::info("[SourceDLSSG NR] retired feature for placement={} passes={} preset={} inputScale={} resolve={} HDR={} producerColor={} peripheral={} fusion={}",
            options.beforeUpscaling ? "before DLSS" : "after DLSS", options.passes, options.reconstruction.preset,
            options.reconstruction.inputScale, static_cast<unsigned>(options.reconstruction.method), options.reconstruction.colorIsHDR,
            options.reconstruction.producerColor, options.reconstruction.peripheralCompression, options.reconstruction.fusedPreparation);
        return true;
    }

    bool Backend::FailNeuralRecording()
    {
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
            !NeuralUnavailableReason(frameNeuralOptions_);
        frameNeuralReset_ = neuralHistory_.ResetFor(frameNeuralOptions_, neuralEligible_,
            reset || (camera && camera->reset == sl::eTrue));
        reset |= frameNeuralReset_;
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
        if (!CopyDepth(depth) || !Check(interop_.CopyInputRegion(motion, motion_, renderExtent), "early NR motion copy") ||
            !Check(interop_.CopyInputRegion(color, earlyNeuralColor_, colorExtent), "early NR world copy") ||
            !Check(interop_.SignalD3D11(Work::Upscaling), "early NR inputs ready")) { return false; }
        ID3D12GraphicsCommandList* list = nullptr;
        if (!Check(interop_.Begin(Work::Upscaling, &list, measure ? &allocatorWait : nullptr), "begin NR before DLSS")) { return false; }
        if (!neuralPass_) { neuralPass_ = std::make_unique<NeuralPass>(); }
        if (!neuralPass_->Record(device12_.Get(), list, interop_.CurrentSlot(Work::Upscaling), frameNeuralOptions_,
            frameNeuralReset_, camera->depthInverted == sl::eTrue,
            camera->mvecScale.x * motion_.desc.Width, camera->mvecScale.y * motion_.desc.Height,
            motion_.texture12.Get(), depth_.texture12.Get(), nullptr, earlyNeuralColor_.texture12.Get(), nullptr,
            neuralTimestampFrequency_)) { return FailNeuralRecording(); }
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
