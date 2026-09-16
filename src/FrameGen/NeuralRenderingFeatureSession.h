#pragma once

#include "NeuralRenderingRuntimeContract.h"

#include <d3d12.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace TheosRenderPipeline::NeuralRendering
{
	class FeatureSession
	{
	public:
		struct CreateInfo
		{
			ID3D12Device* device{ nullptr };
			ID3D12Fence* handoffFence{ nullptr };
			std::uint64_t handoffFenceValue{ 0 };
			std::filesystem::path runtimePath;
			std::uint32_t displayWidth{ 0 };
			std::uint32_t displayHeight{ 0 };
			std::uint32_t renderWidth{ 0 };
			std::uint32_t renderHeight{ 0 };
			// -1 selects the verified runtime's default. Old callers remain legacy-only.
			std::int32_t networkPreset{ -1 };
			bool allowReconstructionRuntime{ false };
		};

		struct EvaluationInput
		{
			ID3D12GraphicsCommandList* commandList{ nullptr };
			ID3D12Resource* color{ nullptr };
			ID3D12Resource* motionVectors{ nullptr };
			ID3D12Resource* depth{ nullptr };
			ID3D12Resource* output{ nullptr };
			ID3D12Resource* backbuffer{ nullptr };
			ID3D12Resource* ui{ nullptr };
			ID3D12Resource* uiAlpha{ nullptr };
			ID3D12Resource* controlMask{ nullptr };
			ID3D12Resource* bidirectionalDistortionField{ nullptr };
			float motionVectorScaleX{ 0.0f };
			float motionVectorScaleY{ 0.0f };
			bool reset{ false };
			bool depthInverted{ false };
			Tuning tuning{};
		};

		FeatureSession();
		~FeatureSession();

		FeatureSession(const FeatureSession&) = delete;
		FeatureSession& operator=(const FeatureSession&) = delete;
		FeatureSession(FeatureSession&&) = delete;
		FeatureSession& operator=(FeatureSession&&) = delete;

		bool EnsureInitialized(const CreateInfo& a_info);
		bool RecordEvaluation(const EvaluationInput& a_input);

		bool IsInitialized() const;
		RuntimeBuild Build() const;
		std::uint64_t EvaluationsRecorded() const;
		const std::string& Status() const;

	private:
		struct State;
		struct StateDeleter
		{
			void operator()(State* state) const;
		};
		std::unique_ptr<State, StateDeleter> state_;
	};
}
