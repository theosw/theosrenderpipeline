#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "D3D11FrameCopy.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace TheosRenderPipeline::SourceDLSSG
{
	// Three allocator slots per work type, independent of the two presentation
	// buffers. A slot may only reset after its recorded GPU submission retires.
	inline constexpr std::size_t kCommandSlots = 3;
	inline constexpr std::size_t kPresentationSlots = 2;
	enum class Work : std::size_t { Upscaling, FrameGeneration, SwapChain, Count };
	inline constexpr const char* WorkName(Work work)
	{
		switch (work) {
		case Work::Upscaling: return "upscaling";
		case Work::FrameGeneration: return "frame-generation";
		case Work::SwapChain: return "swapchain";
		default: return "unavailable";
		}
	}
	struct InteropFailureDiagnostics
	{
		const char* stage{"unavailable"};
		Work work{Work::Count};
		std::size_t slot{};
		std::uint64_t fenceValue{}, submitted{}, waitTarget{}, completed{};
		DWORD timeoutMs{}, waitResult{WAIT_FAILED};
		HRESULT result{S_OK};
		bool completedAvailable{}, waitPerformed{}, valid{};
	};

	struct AllocatorWaitTiming
	{
		std::uint64_t nanoseconds{};
		bool waited{};
	};

	struct SharedTexture
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture11;
		Microsoft::WRL::ComPtr<ID3D12Resource> texture12;
		D3D11_TEXTURE2D_DESC desc{};
	};

	struct InputWaitDiagnostics
	{
		const char* stage{ "not called" };
		// Address snapshots for failure logging only; these do not own COM objects.
		const void* hostIdentity{};
		const void* referenceFenceOwner{};
		const void* inputFenceOwner{};
	};

	class Interop final
	{
	public:
		// All methods run on the host render thread. Owners must keep shared
		// textures alive through a successful Drain before replacing/releasing.
		Interop() = default;
		~Interop();
		Interop(const Interop&) = delete;
		Interop& operator=(const Interop&) = delete;

		// Devices must refer to the same physical adapter. The queue is the
		// presenting queue, not an unrelated worker queue.
		HRESULT Initialize(ID3D11Device* a_device11, ID3D12Device* a_device12,
			ID3D12CommandQueue* a_queue);
		HRESULT CreateSharedTexture(const D3D11_TEXTURE2D_DESC& a_desc, SharedTexture& a_output);
		HRESULT CopyInput(ID3D11Texture2D* a_input, const SharedTexture& a_destination);
		HRESULT CopyInputRegion(ID3D11Texture2D* a_input, const SharedTexture& a_destination, FrameExtent a_extent);
		HRESULT SignalD3D11(Work a_work);
		HRESULT WaitD3D12(Work a_work);
		HRESULT WaitD3D11(Work a_work);
		// Called on the Present thread before any next-frame D3D11 input write.
		// Bridges Streamline's completion fence through our shared fence. With no
		// internal fence, the presenting-queue barrier is valid only with DLSS-G's
		// default eBlockPresentingClientQueue mode.
		HRESULT WaitForInputReaders(ID3D12Fence* a_fence, std::uint64_t a_value);
		const InputWaitDiagnostics& LastInputWait() const { return inputWait_; }
		HRESULT Begin(Work a_work, ID3D12GraphicsCommandList** a_list, AllocatorWaitTiming* a_wait = nullptr);
		HRESULT Submit(Work a_work);
		HRESULT Drain(DWORD a_timeoutMs = 2000);
		HRESULT Fault() const { return fault_; }
		const InteropFailureDiagnostics& LastFailure() const { return failure_; }
		bool Ready() const { return ready_ && SUCCEEDED(fault_); }
		std::uint64_t LastValue(Work a_work) const;
		std::size_t CurrentSlot(Work a_work) const;

		// Both resources enter and leave COMMON.
		static HRESULT RecordCopy(ID3D12GraphicsCommandList* a_list,
			ID3D12Resource* a_source, ID3D12Resource* a_destination);

	private:
		struct WorkContext
		{
			Microsoft::WRL::ComPtr<ID3D12Fence> fence12;
			Microsoft::WRL::ComPtr<ID3D11Fence> fence11;
			std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kCommandSlots> allocators;
			std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>, kCommandSlots> lists;
			std::array<std::uint64_t, kCommandSlots> submitted{};
			std::uint64_t value{ 0 };
			std::size_t slot{ 0 };
			HANDLE event{ nullptr };
			bool recording{ false };
		};
		WorkContext* Get(Work a_work);
		void Observe(const char* stage, Work work = Work::Count);
		HRESULT Check(HRESULT a_result);
		HRESULT WaitCPU(WorkContext& a_work, std::uint64_t a_value, DWORD a_timeoutMs,
			AllocatorWaitTiming* a_timing = nullptr);
		void AbandonInFlightObjects();

		Microsoft::WRL::ComPtr<ID3D11Device5> device11_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;
		Microsoft::WRL::ComPtr<ID3D12Device> device12_;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
		Microsoft::WRL::ComPtr<IUnknown> fenceDeviceIdentity_;
		std::array<WorkContext, static_cast<std::size_t>(Work::Count)> work_;
		InputWaitDiagnostics inputWait_;
		InteropFailureDiagnostics observation_, failure_;
		HRESULT fault_{ S_OK };
		bool ready_{ false };
	};
}
