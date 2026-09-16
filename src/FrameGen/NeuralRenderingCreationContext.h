#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <limits>

namespace TheosRenderPipeline::NeuralRendering
{
	// Separate completion calls let the fixture fail synchronization after a
	// real submission without inducing a driver fault or loading an NR runtime.
	struct CreationCompletion
	{
		HRESULT Signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, std::uint64_t value) const
		{
			return queue->Signal(fence, value);
		}
		HRESULT SetEvent(ID3D12Fence* fence, std::uint64_t value, HANDLE event) const
		{
			return fence->SetEventOnCompletion(value, event);
		}
		DWORD Wait(HANDLE event, DWORD milliseconds) const
		{
			return ::WaitForSingleObject(event, milliseconds);
		}
	};

	// Owned by the feature session until setup has completed. A failed wait
	// does not cancel the command queue or retire its command allocator.
	struct CreationContext
	{
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
		Microsoft::WRL::ComPtr<ID3D12Fence> fence;
		HANDLE fenceEvent{ nullptr };

		CreationContext() = default;
		CreationContext(const CreationContext&) = delete;
		CreationContext& operator=(const CreationContext&) = delete;
		~CreationContext()
		{
			if (fenceEvent) { ::CloseHandle(fenceEvent); }
		}

		bool Initialize(ID3D12Device* device)
		{
			if (!device) { return false; }
			D3D12_COMMAND_QUEUE_DESC description{};
			description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue))) ||
				FAILED(device->CreateCommandAllocator(description.Type, IID_PPV_ARGS(&allocator))) ||
				FAILED(device->CreateCommandList(0, description.Type, allocator.Get(), nullptr,
					IID_PPV_ARGS(&commandList))) ||
				FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
				return false;
			}
			fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			return fenceEvent != nullptr;
		}

		template <class Completion = CreationCompletion>
		bool SubmitAndWait(ID3D12Fence* handoffFence, std::uint64_t handoffValue,
			DWORD milliseconds = 30000, const Completion& completion = {})
		{
			if (submitted_) { return false; }
			if (handoffFence && handoffValue != 0 && FAILED(queue->Wait(handoffFence, handoffValue))) {
				return false;
			}
			if (FAILED(commandList->Close())) { return false; }
			ID3D12CommandList* lists[]{ commandList.Get() };
			queue->ExecuteCommandLists(1, lists);
			submitted_ = true;
			if (FAILED(completion.Signal(queue.Get(), fence.Get(), kFenceValue))) { return false; }
			signalQueued_ = true;
			if (FAILED(completion.SetEvent(fence.Get(), kFenceValue, fenceEvent)) ||
				completion.Wait(fenceEvent, milliseconds) != WAIT_OBJECT_0) { return false; }
			return Completed();
		}

		bool CanRelease() const { return !submitted_ || Completed(); }

	private:
		bool Completed() const
		{
			if (!signalQueued_) { return false; }
			const auto value = fence->GetCompletedValue();
			// UINT64_MAX means device removal, not successful feature creation.
			return value != (std::numeric_limits<std::uint64_t>::max)() && value >= kFenceValue;
		}
		static constexpr std::uint64_t kFenceValue = 1;
		bool submitted_{ false };
		bool signalQueued_{ false };
	};

	// Keep the entire failed session (feature, parameters, module reference and
	// creation objects) if its owner goes away with setup still outstanding.
	// The terminal caller requires restart; unconfirmed work is retained for
	// process lifetime, with no background retries or destructor-time wait.
	template <class State>
	bool DestroyAfterCreation(State* state)
	{
		if (state->creationContext && !state->creationContext->CanRelease()) { return false; }
		delete state;
		return true;
	}
}
