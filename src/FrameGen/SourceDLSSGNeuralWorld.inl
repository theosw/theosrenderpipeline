// Shared implementation of the world stage. Included by the production backend
// and its CPU execution fixture; device operations are supplied by the owner.
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
        ApplyNeuralRetry(options, options.WorldOnly() ? colorExtent :
            FrameExtent{presenterDesc_.BufferDesc.Width, presenterDesc_.BufferDesc.Height});
        frameNeuralOptions_ = options;
        // Preserve the saved after-DLSS preference while enforcing a world-only
        // input contract for the early stage. There are no UI pixels to correct.
        if (frameNeuralOptions_.WorldOnly()) { frameNeuralOptions_.tuning.uiCorrection = false; }
        neuralFrameBegun_ = true;
        neuralEvaluatedEarly_ = false;
        frameNeuralModelChanged_ = neuralPass_ && neuralPass_->ModelChangePending(frameNeuralOptions_);
        // Pascal replacement admission is serial: retire first, then query actual
        // DXGI usage before opening any list or allocating the replacement.
        const bool canAttempt = options.enabled && eligible && !neuralTransitionBlocked_.load(std::memory_order_acquire) && !neuralRecovery_.Disabled();
        const bool replacement = canAttempt && neuralCapability_.Pascal() &&
            !NeuralRendering::PascalResearchFor(neuralComputeCapability_) && neuralPass_ &&
            neuralPass_->NeedsRecreation(frameNeuralOptions_, renderExtent.width, renderExtent.height);
        if (!NeuralRendering::PrepareNeuralAdmission(replacement,
                [&] { return RecreateNeuralIfNeeded(frameNeuralOptions_, renderExtent); }, [&] {
            neuralEligible_ = eligible && !neuralTransitionBlocked_.load(std::memory_order_acquire) && (!options.WorldOnly() || camera) &&
                !NeuralUnavailableReason(frameNeuralOptions_, options.WorldOnly() ? colorExtent :
                    FrameExtent{presenterDesc_.BufferDesc.Width, presenterDesc_.BufferDesc.Height});
        })) { return false; }
        frameNeuralReset_ = neuralHistory_.ResetFor(frameNeuralOptions_, neuralEligible_,
            reset || (camera && camera->reset == sl::eTrue));
        frameNeuralReset_ |= std::exchange(neuralRecoveryReset_, false);
        reset |= frameNeuralReset_;
        // The upscaler still needs to discard a history fed by a changed NR
        // model. Keep this separate from the NR chain's per-pass reset mask.
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
        if (!CopyObjectMask(renderExtent) || !CopyDepth(depth) || !CopyGuide(motion, motion_, renderExtent, "early NR motion copy") ||
            !CopyGuide(color, earlyNeuralColor_, colorExtent, "early NR world copy") ||
            !Check(interop_.SignalD3D11(Work::Upscaling), "early NR inputs ready")) { return false; }
        ID3D12GraphicsCommandList* list = nullptr;
        const auto begun=interop_.Begin(Work::Upscaling,&list,measure?&allocatorWait:nullptr,
            frameNeuralOptions_.backend==NeuralRendering::Backend::Vulkan);
        if(begun==S_FALSE) { reset=true;neuralHistory_.Invalidate();return true; }
        if(!Check(begun,"begin NR before DLSS")) { return false; }
        bool recorded{};
        const bool wasPreparing = neuralPass_ && neuralPass_->Preparing();
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
                std::array<float,2>{}, FrameIndex(), &interop_, queue_.Get());
        } catch (const std::bad_alloc&) { reset = true; return FailNeuralRecording("NR CPU allocation failed"); }
        if (!recorded && neuralPass_->Preparing()) {
            if (!Check(interop_.Discard(Work::Upscaling), "discard pending Vulkan NR preparation")) return false;
            std::scoped_lock lock(neuralMutex_);
            neuralSnapshot_.active = false;
            neuralSnapshot_.status = NeuralRendering::kPreparingVulkan;
            // Keep the upscaler history while preparation runs in the background.
            return true;
        }
        reset |= wasPreparing; // first evaluated NR frame changes the upscaler input

        if (!recorded) {
            // Producer colour was never overwritten; discard every transition
            // and inference command, then let reconstruction consume that colour.
            const bool recovered = FailNeuralRecording();
            reset = true;
            return recovered;
        }
        list = interop_.CurrentList(Work::Upscaling); // Vulkan hands off to a distinct continuation.
        if (!Check(Interop::RecordCopy(list, neuralPass_->Corrected(), earlyNeuralColor_.texture12.Get()), "early NR result copy") ||
            !Check(interop_.Submit(Work::Upscaling), "submit NR before DLSS") ||
            !Check(neuralPass_->Submitted(queue_.Get()) ? S_OK : E_FAIL, "notify AMD NR after host submission") ||
            !Check(interop_.WaitD3D11(Work::Upscaling), "DLSS waits for NR")) { return false; }
        if (neuralPass_->AMDFailed()) {
            // The producer's image is still untouched. Retire the AMD owner
            // through the same safe-off/Apply-retry boundary as native NR.
            reset = true;
            return FailNeuralRecording();
        }
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
