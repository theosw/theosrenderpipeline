// Included by SourceDLSSGNeuralRendering.cpp inside its namespace. Both feature
// histories evaluate once per active source frame. No CPU wait is introduced here.
bool NeuralPass::RecordSecond(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
	const NeuralOptions& options, const NeuralRendering::FeatureSession::EvaluationInput& first,
	const ResolveConstants& originalConstants, ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* ui,
	NeuralRendering::FeatureSession::EvaluationInput& second)
{
	constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
	constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	auto constants = originalConstants;
	auto dispatch = [&](unsigned stage, ResolveKernel kernel, ID3D12Resource* a, ID3D12Resource* b,
		ID3D12Resource* original, ID3D12Resource* output, ID3D12Resource* extra = nullptr) {
		const auto hr = resolve_.Record(device, list, slot, stage, kernel, constants, a, b, original, output, extra);
		if (FAILED(hr)) { status_ = std::format("NR pass 2 preparation failed 0x{:08X}", static_cast<UINT>(hr)); }
		return SUCCEEDED(hr);
	};
	Transition(list, first.output, write, common);
	second = first;
	second.color = first.output;
	second.output = secondOutput_.Get();
	second.tuning = options.EffectiveSecond().tuning;
	second.reset = first.reset || secondHistoryInvalid_ || secondFeature_.EvaluationsRecorded() == 0;
	if (secondInput_) {
		// Already encoded/warped model colour: resize it without encoding or
		// applying the peripheral mapping a second time.
		constants.sourceWidth = static_cast<UINT>(first.output->GetDesc().Width);
		constants.sourceHeight = first.output->GetDesc().Height;
		constants.targetWidth = static_cast<UINT>(second.output->GetDesc().Width);
		constants.targetHeight = second.output->GetDesc().Height;
		constants.peripheral = 0;
		if (!dispatch(7, ResolveKernel::ResizeColor, first.output, nullptr, nullptr, secondInput_.Get())) { return false; }
		second.color = secondInput_.Get();
		if (secondMotion_) {
			// Repack the original guides, including motion endpoints, at pass 2's
			// own model extent. Reusing pass 1's packed vectors changes their units.
			constants = originalConstants;
			constants.targetWidth = constants.workWidth = static_cast<UINT>(second.output->GetDesc().Width);
			constants.targetHeight = constants.workHeight = second.output->GetDesc().Height;
			if (!dispatch(8, ResolveKernel::PackGuides, depth, motion, nullptr, secondDepth_.Get(), secondMotion_.Get())) { return false; }
			second.depth = secondDepth_.Get(); second.motionVectors = secondMotion_.Get();
			for (auto* r : { second.depth, second.motionVectors }) { Transition(list, r, common, read); }
			if (ui) {
				if (!dispatch(9, ResolveKernel::Downsample, ui, nullptr, nullptr, secondUI_.Get())) { return false; }
				second.ui = secondUI_.Get(); Transition(list, second.ui, common, read);
			}
		} else {
			const auto ratio = float(second.output->GetDesc().Width) / originalConstants.sourceWidth;
			second.motionVectorScaleX = originalConstants.motionScaleX * ratio;
			second.motionVectorScaleY = originalConstants.motionScaleY * ratio;
		}
	}
	if (NeuralRendering::UsesReconstructionContract(secondFeature_.Build()) && second.tuning.uiCorrection &&
		FAILED(Interop::RecordCopy(list, second.color, second.output))) {
		status_ = "NR second-pass background copy rejected"; return false;
	}
	Transition(list, second.color, common, read);
	Transition(list, second.output, common, write);
	second.backbuffer = NeuralRendering::UsesReconstructionContract(secondFeature_.Build()) ? second.output : first.backbuffer;
	if (!secondFeature_.RecordEvaluation(second)) { status_ = "NR second pass: " + secondFeature_.Status(); return false; }
	secondHistoryInvalid_ = false;
	return true;
}

// Keep final restoration/copy outside the existing inference timing boundary.
bool NeuralPass::FinishSecond(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
	const NeuralRendering::FeatureSession::EvaluationInput& first,
	const NeuralRendering::FeatureSession::EvaluationInput& second, const ResolveConstants& originalConstants)
{
	constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
	constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	auto constants = originalConstants;
	auto dispatch = [&](unsigned stage, ResolveKernel kernel, ID3D12Resource* a, ID3D12Resource* b,
		ID3D12Resource* original, ID3D12Resource* output, ID3D12Resource* extra = nullptr) {
		const auto hr = resolve_.Record(device, list, slot, stage, kernel, constants, a, b, original, output, extra);
		if (FAILED(hr)) { status_ = std::format("NR pass 2 preparation failed 0x{:08X}", static_cast<UINT>(hr)); }
		return SUCCEEDED(hr);
	};
	Transition(list, second.color, read, common);
	Transition(list, second.output, write, common);
	if (secondMotion_) {
		for (auto* r : { second.depth, second.motionVectors, second.ui }) { Transition(list, r, read, common); }
	}
	auto* result = second.output;
	if (secondInput_) {
		// Transfer only pass 2's RGB change back to pass 1's grid. This preserves
		// first-pass detail/alpha even when the second model operates at 25%.
		constants.sourceWidth = static_cast<UINT>(second.output->GetDesc().Width);
		constants.sourceHeight = second.output->GetDesc().Height;
		constants.targetWidth = static_cast<UINT>(first.output->GetDesc().Width);
		constants.targetHeight = first.output->GetDesc().Height;
		constants.peripheral = 0;
		if (!dispatch(10, ResolveKernel::RestoreSecond, second.color, second.output, first.output, secondRestored_.Get())) { return false; }
		result = secondRestored_.Get();
	}
	// Keep the established final resolve/composition endpoint stable. Copy
	// elimination can be evaluated independently of these new controls.
	if (FAILED(Interop::RecordCopy(list, result, first.output))) { status_ = "NR second-pass result copy rejected"; return false; }
	Transition(list, first.output, common, write);
	return true;
}
