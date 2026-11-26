// Included inside SourceDLSSG. All reconstruction and temporal operations use
// the same kernels as the synchronous/model-rate path. Only job ownership differs.
bool NeuralPass::RecordVulkanBackground(ID3D12Device* device, ID3D12GraphicsCommandList*& list,
	std::size_t slot, const NeuralOptions& options, bool reset, bool depthInverted, float scaleX, float scaleY,
	ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* hudless, std::array<float, 2> jitter,
	std::optional<std::uint32_t> applicationFrame, Interop* interop, ID3D12CommandQueue* queue)
{
	if (!interop || !queue) {
		status_ = "Vulkan background submission context missing";
		return false;
	}
	auto& bg = background_;
	const auto frame = ++bg.frame;
	reset |= rateContinuity_.Record(rateClock_(), applicationFrame);
	reset |= !SameModelOutputOptions(options, rateOptions_);
	rateOptions_ = options;
	if (reset) {
		bg.schedule.Invalidate();
		bg.forceReset = true;
	}
	bool ready{};
	if (!feature_.PollVulkan(slot, queue, ready)) {
		status_ = feature_.Status();
		return false;
	}
	const auto completion = bg.schedule.Complete(frame, ready);
	if (completion.dropped) {
		if (!feature_.DropVulkan(queue)) {
			status_ = feature_.Status();
			return false;
		}
		bg.forceReset = true;
	}
	if (!EnsureModelRate(device, motion, hudless)) { return false; }
	// The ordinary cadence path already owns two guide textures; extend that
	// set by one instead of allocating five full-resolution guides.
	if (!bg.history[0].guides) {
		bg.history[0].guides = rateGuides_[0];
		bg.history[1].guides = rateGuides_[1];
	}
	auto allocate = [&](ID3D12Resource* like, Microsoft::WRL::ComPtr<ID3D12Resource>& out,
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN) {
		if (out) { return true; }
		const auto desc = like->GetDesc();
		const auto fmt = format != DXGI_FORMAT_UNKNOWN ? format : desc.Format;
		if (FAILED(D3D12Helpers::CreateDefaultTexture2D(device, desc.Width, desc.Height, fmt,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &out))) {
			status_ = "Vulkan background history allocation failed";
			return false;
		}
		return true;
	};
	auto copy = [&](ID3D12Resource* from, ID3D12Resource* to) {
		if (FAILED(Interop::RecordCopy(list, from, to))) {
			status_ = "Vulkan background history copy rejected";
			return false;
		}
		return true;
	};
	auto& current = bg.history[frame % bg.history.size()];
	if (!allocate(hudless, current.colour) || !allocate(motion, current.guides, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
		return false;
	}
	current.serial = frame;
	current.jitter = jitter;
	for (auto& value : current.jitter) {
		if (!std::isfinite(value)) { value = 0.0f; }
	}
	const auto reconstruction = NeuralRendering::SanitizeReconstruction(options.reconstruction);
	const auto method = NeuralRendering::EffectiveResolve(reconstruction);
	const bool resolving = method != NeuralRendering::ResolveMethod::Auto;
	auto* output = resolving ? workOutput_.Get() : corrected_.Get();

	ResolveConstants c;
	c.sourceWidth = static_cast<UINT>(hudless->GetDesc().Width);
	c.sourceHeight = hudless->GetDesc().Height;
	c.targetWidth = c.workWidth = static_cast<UINT>(output->GetDesc().Width);
	c.targetHeight = c.workHeight = output->GetDesc().Height;
	c.guideWidth = static_cast<UINT>(motion->GetDesc().Width);
	c.guideHeight = motion->GetDesc().Height;
	c.motionScaleX = scaleX;
	c.motionScaleY = scaleY;
	c.depthInverted = depthInverted;
	c.cameraJitterX = current.jitter[0];
	c.cameraJitterY = current.jitter[1];
	c.passthrough = !reconstruction.colorIsHDR;
	c.producerColor = reconstruction.producerColor;
	c.transferStrength = reconstruction.transferStrength;
	c.colourStrength = reconstruction.colourStrength;
	c.maxRatio = reconstruction.maxRatio;
	c.whitePoint = reconstruction.whitePoint;
	c.encodedFormat = hudless->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : 2;
	c.changeFloor = reconstruction.producerColor ? 0.02f * c.whitePoint
	              : reconstruction.colorIsHDR    ? 0.002f * c.whitePoint
	                                             : 0.02f;
	unsigned stage{}; // Each recording owns a distinct descriptor range, even for repeated reprojection.
	auto dispatch = [&](ResolveKernel kernel, ID3D12Resource* a, ID3D12Resource* b,
		ID3D12Resource* original, ID3D12Resource* out, ID3D12Resource* fourth = nullptr) {
		const auto hr = resolve_.Record(device, list, slot, stage++, kernel, c, a, b, original, out, nullptr, fourth);
		if (FAILED(hr)) {
			status_ = std::format("Vulkan background reconstruction failed 0x{:08X}", static_cast<UINT>(hr));
			return false;
		}
		return true;
	};
	const bool timing = timestamps_ && timestampFrequency_;
	const auto query = static_cast<UINT>(slot * 4);
	if (timing) { list->EndQuery(timestamps_.Heap(), D3D12_QUERY_TYPE_TIMESTAMP, query + 2); }
	if (!copy(hudless, current.colour.Get()) ||
		!dispatch(ResolveKernel::CaptureGuides, motion, depth, nullptr, current.guides.Get())) {
		return false;
	}

	// A finished result belongs to its capture colour/exposure, never this frame's.
	// Reconstruct there, store its change, then walk the existing one-frame
	// reprojection through every retained guide pair to the current frame.
	std::uint64_t from = frame - 1;
	if (completion.consume) {
		const auto job = *completion.consume;
		auto& capture = bg.history[job.frame % bg.history.size()];
		if (!bg.schedule.Valid(job, frame) || capture.serial != job.frame) {
			status_ = "Vulkan background capture history unavailable";
			return false;
		}
		NeuralRendering::FeatureSession::EvaluationInput input;
		input.color = bg.modelInput.Get();
		input.output = output;
		input.motionVectors = motion;
		input.depth = depth;
		if (!feature_.ConsumeVulkan(input, queue, slot, list)) {
			status_ = feature_.Status();
			return false;
		}
		const auto currentConstants = c;
		c = bg.captured;
		resolve_.SetExposure(c.adaptiveExposure ? bg.exposure.Get() : nullptr);
		if (resolving) {
			const unsigned stage0 = stage;
			const unsigned stage1 = stage + 1;
			const unsigned toneBase = stage;
			const unsigned ratioStage = stage + 3;
			stage += 4;
			if (!RecordResolveOutput(device, list, slot, reconstruction, method, bg.modelInput.Get(), output,
					capture.colour.Get(), c, capture.jitter, true, stage0, stage1, toneBase, ratioStage)) {
				resolve_.SetExposure(nullptr);
				return false;
			}
		}
		resolve_.SetExposure(nullptr);
		c = currentConstants;
		if (!dispatch(ResolveKernel::StoreChange, capture.colour.Get(), corrected_.Get(), nullptr, rateChange_.Get())) {
			return false;
		}
		bg.schedule.Applied();
		from = job.frame;
	}
	if (bg.schedule.HasHistory()) {
		for (auto next = from + 1; next <= frame; ++next) {
			auto& previous = bg.history[(next - 1) % bg.history.size()];
			auto& target = bg.history[next % bg.history.size()];
			if (previous.serial != next - 1 || target.serial != next) {
				status_ = "Vulkan reprojection history discontinuity";
				return false;
			}
			c.cameraJitterX = target.jitter[0];
			c.cameraJitterY = target.jitter[1];
			c.previousJitterX = previous.jitter[0];
			c.previousJitterY = previous.jitter[1];
			if (!dispatch(ResolveKernel::Reproject, target.colour.Get(), rateChange_.Get(), target.guides.Get(),
					corrected_.Get(), previous.guides.Get()) ||
				!dispatch(ResolveKernel::StoreChange, target.colour.Get(), corrected_.Get(), nullptr, rateChange_.Get())) {
				return false;
			}
		}
	} else if (!copy(hudless, corrected_.Get())) {
		return false; // Identical first/reset-frame fallback.
	}

	// Prepare every frame, including frames with an outstanding job. This keeps
	// the host preparation cadence uniform; the immutable submitted copy is separate.
	c.mode = 0;
	c.targetWidth = c.workWidth;
	c.targetHeight = c.workHeight;
	c.cameraJitterX = current.jitter[0];
	c.cameraJitterY = current.jitter[1];
	if (!RecordAdaptiveExposure(device, list, slot, stage++, resolving, reconstruction, reset, hudless, c)) {
		return false;
	}
	auto* colour = hudless;
	if (resolving) {
		const bool steady = method == NeuralRendering::ResolveMethod::Ratio;
		if (encoded_) {
			c.targetWidth = c.sourceWidth;
			c.targetHeight = c.sourceHeight;
			c.jitterX = steady ? current.jitter[0] : 0.0f;
			c.jitterY = steady ? current.jitter[1] : 0.0f;
			if (!dispatch(ResolveKernel::Ratio, hudless, nullptr, nullptr, encoded_.Get())) { return false; }
			colour = encoded_.Get();
			c.jitterX = c.jitterY = 0.0f;
		}
		if (workColor_) {
			c.targetWidth = c.workWidth;
			c.targetHeight = c.workHeight;
			c.jitterX = (steady && colour == hudless) ? current.jitter[0] : 0.0f;
			c.jitterY = (steady && colour == hudless) ? current.jitter[1] : 0.0f;
			if (!dispatch(fusedColor_ ? ResolveKernel::PrepareColor : ResolveKernel::Downsample,
					colour, nullptr, nullptr, workColor_.Get())) {
				return false;
			}
			colour = workColor_.Get();
			c.jitterX = c.jitterY = 0.0f;
		}
	}
	// Background motion composition covers at most a 2-frame gap; cap interval accordingly
	// so EveryThirdFrame does not reset temporal history on every evaluation.
	const auto interval = (std::min)(
		NeuralRendering::ModelRateInterval(NeuralRendering::EffectiveModelRate(reconstruction.modelRate)), 2u);
	const bool submit = bg.schedule.CanSubmit(frame, interval);
	if (submit) {
		if (!allocate(colour, bg.modelInput) || !copy(colour, bg.modelInput.Get())) { return false; }
		if (c.adaptiveExposure && (!allocate(exposure_.Get(), bg.exposure) || !copy(exposure_.Get(), bg.exposure.Get()))) {
			return false;
		}
		bg.captured = c;
		NeuralRendering::FeatureSession::EvaluationInput input;
		input.color = bg.modelInput.Get();
		input.output = output;
		input.motionVectors = motion;
		input.depth = depth;
		const auto gap = frame - bg.lastCapture;
		input.reset = bg.forceReset || gap > 2;
		if (!input.reset && gap == 2) {
			auto& previous = bg.history[(frame - 1) % bg.history.size()];
			c.previousJitterX = previous.jitter[0];
			c.previousJitterY = previous.jitter[1];
			if (!dispatch(ResolveKernel::ComposeMotion, motion, current.guides.Get(), previous.guides.Get(), rateMotion_.Get())) {
				return false;
			}
			input.motionVectors = rateMotion_.Get();
		}
		input.motionVectorScaleX = scaleX * static_cast<float>(c.workWidth) / c.sourceWidth;
		input.motionVectorScaleY = scaleY * static_cast<float>(c.workHeight) / c.sourceHeight;
		input.depthInverted = depthInverted;
		input.tuning = options.tuning;
		input.commandList = list;
		if (!feature_.EvaluateVulkan(input, *interop, queue, slot, list, true)) {
			status_ = feature_.Status();
			return false;
		}
		if (!bg.schedule.Submit(frame, interval)) {
			status_ = "Vulkan scheduler reservation failed";
			return false;
		}
		bg.lastCapture = frame;
		bg.forceReset = false;
	}
	// Compare never contaminates stored NR change.
	if (options.compare.split) {
		if (!allocate(corrected_.Get(), splitResult_)) { return false; }
		c.targetWidth = c.sourceWidth;
		c.targetHeight = c.sourceHeight;
		c.splitX = NeuralRendering::SplitColumn(options.compare, c.sourceWidth);
		if (!dispatch(ResolveKernel::Split, hudless, corrected_.Get(), nullptr, splitResult_.Get()) ||
			!copy(splitResult_.Get(), corrected_.Get())) {
			return false;
		}
	}
	telemetry_.RecordModelRun(submit);
	modelActivity_ = submit ? NRModelActivity::Ran : NRModelActivity::Skipped;
	if (timing) {
		list->EndQuery(timestamps_.Heap(), D3D12_QUERY_TYPE_TIMESTAMP, query + 3);
		list->ResolveQueryData(timestamps_.Heap(), D3D12_QUERY_TYPE_TIMESTAMP, query + 2, 2,
			timestamps_.Readback(), sizeof(std::uint64_t) * (query + 2));
		pendingTiming_[slot] = { feature_.EvaluationsRecorded(), 0, true, false };
	}
	status_ = "Vulkan background; one job maximum; result reprojected from capture; experimental pacing";
	if (frame % 120 == 0) {
		const auto measured = feature_.VulkanMeasurements();
		logger::info("[Vulkan NR] background frame={} resultAge={} arrivalAge={} inFlight={} dropped/stale={} network={:.3f} ms copies/frame={:.3f} ms",
			frame, bg.schedule.DisplayedAge(frame), bg.schedule.ResultAge(), bg.schedule.InFlight(), bg.schedule.Dropped(),
			measured.NetworkMs(), measured.CopiesPerFrameMs());
	}
	return true;
}
