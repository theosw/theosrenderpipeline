#include "SourceDLSSGInterop.h"

#include <dxgi1_4.h>
#include <chrono>
#include <limits>
#include <utility>

namespace TheosRenderPipeline::SourceDLSSG
{
	Interop::~Interop()
	{
		if (FAILED(Drain())) {
			// A timeout is not retirement. Keep GPU-owned objects alive until the
			// process ends rather than freeing an allocator that is still in use.
			AbandonInFlightObjects();
		}
		for (auto& work : work_) {
			if (work.event) {
				::CloseHandle(work.event);
			}
		}
	}

	HRESULT Interop::Check(HRESULT a_result)
	{
		if (FAILED(a_result) && SUCCEEDED(fault_)) {
			fault_ = a_result;
		}
		return a_result;
	}

	Interop::WorkContext* Interop::Get(Work a_work)
	{
		const auto index = static_cast<std::size_t>(a_work);
		return index < work_.size() ? &work_[index] : nullptr;
	}

	HRESULT Interop::Initialize(ID3D11Device* a_device11, ID3D12Device* a_device12,
		ID3D12CommandQueue* a_queue)
	{
		if (device11_ || !a_device11 || !a_device12 || !a_queue) {
			return E_INVALIDARG;
		}
		Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
		Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC adapterDesc{};
		HRESULT hr = a_device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
		if (FAILED(hr)) { return Check(hr); }
		if (FAILED(hr = dxgiDevice->GetAdapter(&adapter))) { return Check(hr); }
		if (FAILED(hr = adapter->GetDesc(&adapterDesc))) { return Check(hr); }
		const auto luid = a_device12->GetAdapterLuid();
		if (luid.HighPart != adapterDesc.AdapterLuid.HighPart ||
			luid.LowPart != adapterDesc.AdapterLuid.LowPart ||
			a_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
			return E_INVALIDARG;
		}
		Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
		Microsoft::WRL::ComPtr<IUnknown> suppliedIdentity, queueIdentity;
		if (FAILED(hr = a_queue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
			FAILED(hr = a_device12->QueryInterface(IID_PPV_ARGS(&suppliedIdentity))) ||
			FAILED(hr = queueDevice.As(&queueIdentity))) { return Check(hr); }
		if (suppliedIdentity.Get() != queueIdentity.Get()) { return E_INVALIDARG; }
		if (FAILED(hr = a_device11->QueryInterface(IID_PPV_ARGS(&device11_)))) { return Check(hr); }
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
		a_device11->GetImmediateContext(&immediate);
		if (FAILED(hr = immediate.As(&context11_))) { return Check(hr); }
		device12_ = a_device12;
		queue_ = a_queue;
		for (auto& work : work_) {
			if (FAILED(hr = device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
				IID_PPV_ARGS(&work.fence12)))) { return Check(hr); }
			HANDLE handle = nullptr;
			hr = device12_->CreateSharedHandle(work.fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
			if (FAILED(hr)) { return Check(hr); }
			hr = device11_->OpenSharedFence(handle, IID_PPV_ARGS(&work.fence11));
			::CloseHandle(handle);
			if (FAILED(hr)) { return Check(hr); }
			work.event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!work.event) { return Check(HRESULT_FROM_WIN32(::GetLastError())); }
			for (std::size_t slot = 0; slot < kCommandSlots; ++slot) {
				if (FAILED(hr = device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
					IID_PPV_ARGS(&work.allocators[slot])))) { return Check(hr); }
				if (FAILED(hr = device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					work.allocators[slot].Get(), nullptr, IID_PPV_ARGS(&work.lists[slot])))) { return Check(hr); }
				if (FAILED(hr = work.lists[slot]->Close())) { return Check(hr); }
			}
		}
		ready_ = true;
		return S_OK;
	}

	HRESULT Interop::CreateSharedTexture(const D3D11_TEXTURE2D_DESC& a_desc, SharedTexture& a_output)
	{
		if (!Ready()) { return FAILED(fault_) ? fault_ : E_UNEXPECTED; }
		if (a_output.texture11 || a_output.texture12 ||
			a_desc.Usage != D3D11_USAGE_DEFAULT || a_desc.CPUAccessFlags ||
			(a_desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) ||
			!a_desc.Width || !a_desc.Height || a_desc.MipLevels != 1 || a_desc.ArraySize != 1 ||
			a_desc.SampleDesc.Count != 1 || a_desc.SampleDesc.Quality != 0 ||
			a_desc.Format == DXGI_FORMAT_UNKNOWN) { return E_INVALIDARG; }
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = a_desc.Width;
		desc.Height = a_desc.Height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = a_desc.Format;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		if (a_desc.BindFlags & D3D11_BIND_RENDER_TARGET) { desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; }
		if (a_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) { desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; }
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		SharedTexture result;
		auto hr = device12_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED,
			&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result.texture12));
		if (FAILED(hr)) { return Check(hr); }
		HANDLE handle = nullptr;
		hr = device12_->CreateSharedHandle(result.texture12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if (FAILED(hr)) { return Check(hr); }
		hr = device11_->OpenSharedResource1(handle, IID_PPV_ARGS(&result.texture11));
		::CloseHandle(handle);
		if (FAILED(hr)) { return Check(hr); }
		result.texture11->GetDesc(&result.desc);
		a_output = std::move(result);
		return S_OK;
	}

	HRESULT Interop::CopyInput(ID3D11Texture2D* a_input, const SharedTexture& a_destination)
	{
		if (!Ready()) { return FAILED(fault_) ? fault_ : E_UNEXPECTED; }
		if (!a_input || !a_destination.texture11 || a_input == a_destination.texture11.Get()) { return E_INVALIDARG; }
		Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice, destinationDevice;
		a_input->GetDevice(&sourceDevice);
		a_destination.texture11->GetDevice(&destinationDevice);
		if (sourceDevice.Get() != static_cast<ID3D11Device*>(device11_.Get()) ||
			destinationDevice.Get() != sourceDevice.Get()) { return E_INVALIDARG; }
		D3D11_TEXTURE2D_DESC desc{};
		a_input->GetDesc(&desc);
		if (desc.Width != a_destination.desc.Width || desc.Height != a_destination.desc.Height ||
			desc.Format != a_destination.desc.Format || desc.ArraySize != 1 || desc.MipLevels != 1 ||
			desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0) { return E_INVALIDARG; }
		context11_->CopyResource(a_destination.texture11.Get(), a_input);
		return S_OK;
	}

	HRESULT Interop::CopyInputRegion(ID3D11Texture2D* a_input, const SharedTexture& a_destination, FrameExtent a_extent)
	{
		if (!Ready()) { return FAILED(fault_) ? fault_ : E_UNEXPECTED; }
		if (a_destination.desc.Width != a_extent.width || a_destination.desc.Height != a_extent.height) { return E_INVALIDARG; }
		return D3D11FrameCopy::Color(context11_.Get(), a_input, a_destination.texture11.Get(), a_extent);
	}

	HRESULT Interop::SignalD3D11(Work a_work)
	{
		auto* work = Get(a_work);
		if (!Ready() || !work || work->recording) { return E_UNEXPECTED; }
		if (work->value) {
			const auto wait = context11_->Wait(work->fence11.Get(), work->value);
			if (FAILED(wait)) { return Check(wait); }
		}
		const auto hr = context11_->Signal(work->fence11.Get(), ++work->value);
		context11_->Flush();
		return Check(hr);
	}

	HRESULT Interop::WaitD3D12(Work a_work)
	{
		auto* work = Get(a_work);
		if (!Ready() || !work) { return E_UNEXPECTED; }
		return work->value ? Check(queue_->Wait(work->fence12.Get(), work->value)) : S_OK;
	}

	HRESULT Interop::WaitD3D11(Work a_work)
	{
		auto* work = Get(a_work);
		if (!Ready() || !work) { return E_UNEXPECTED; }
		return work->value ? Check(context11_->Wait(work->fence11.Get(), work->value)) : S_OK;
	}

	HRESULT Interop::WaitForInputReaders(ID3D12Fence* a_fence, std::uint64_t a_value)
	{
		auto* work = Get(Work::FrameGeneration);
		if (!Ready() || work->recording || (!a_fence && a_value)) { return E_UNEXPECTED; }
		if (a_fence) {
			Microsoft::WRL::ComPtr<ID3D12Device> owner;
			Microsoft::WRL::ComPtr<IUnknown> ownerIdentity, deviceIdentity;
			auto hr = a_fence->GetDevice(IID_PPV_ARGS(&owner));
			if (FAILED(hr)) { return Check(hr); }
			if (FAILED(hr = owner.As(&ownerIdentity)) || FAILED(hr = device12_.As(&deviceIdentity))) { return Check(hr); }
			if (ownerIdentity != deviceIdentity) { return E_INVALIDARG; }
		}
		auto hr = WaitD3D12(Work::FrameGeneration);
		if (FAILED(hr)) { return hr; }
		if (a_fence && a_value && FAILED(hr = queue_->Wait(a_fence, a_value))) { return Check(hr); }
		// This queue signal also follows the most recent native Present. It
		// covers the default DLSS-G presenting-queue block when no fence is given.
		hr = queue_->Signal(work->fence12.Get(), ++work->value);
		if (FAILED(hr)) { return Check(hr); }
		return WaitD3D11(Work::FrameGeneration);
	}

	HRESULT Interop::WaitCPU(WorkContext& a_work, std::uint64_t a_value, DWORD a_timeoutMs,
		AllocatorWaitTiming* a_timing)
	{
		if (!a_value) { return S_OK; }
		const auto completed = a_work.fence12->GetCompletedValue();
		if (completed == (std::numeric_limits<std::uint64_t>::max)()) {
			return Check(DXGI_ERROR_DEVICE_REMOVED);
		}
		if (completed >= a_value) { return S_OK; }
		const auto begin = a_timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		auto hr = a_work.fence12->SetEventOnCompletion(a_value, a_work.event);
		if (FAILED(hr)) { return Check(hr); }
		const auto wait = ::WaitForSingleObject(a_work.event, a_timeoutMs);
		if (a_timing) {
			a_timing->waited = true;
			a_timing->nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - begin).count());
		}
		if (wait == WAIT_TIMEOUT) { return Check(HRESULT_FROM_WIN32(WAIT_TIMEOUT)); }
		if (wait != WAIT_OBJECT_0) { return Check(HRESULT_FROM_WIN32(::GetLastError())); }
		const auto retired = a_work.fence12->GetCompletedValue();
		if (retired == (std::numeric_limits<std::uint64_t>::max)()) { return Check(DXGI_ERROR_DEVICE_REMOVED); }
		return retired >= a_value ? S_OK : Check(E_FAIL);
	}

	HRESULT Interop::Begin(Work a_work, ID3D12GraphicsCommandList** a_list, AllocatorWaitTiming* a_wait)
	{
		if (a_wait) { *a_wait = {}; }
		if (!a_list) { return E_POINTER; }
		*a_list = nullptr;
		auto* work = Get(a_work);
		if (!Ready() || !work || work->recording) { return E_UNEXPECTED; }
		auto hr = WaitD3D12(a_work);
		if (FAILED(hr)) { return hr; }
		if (FAILED(hr = WaitCPU(*work, work->submitted[work->slot], 2000, a_wait))) { return hr; }
		if (FAILED(hr = work->allocators[work->slot]->Reset())) { return Check(hr); }
		if (FAILED(hr = work->lists[work->slot]->Reset(work->allocators[work->slot].Get(), nullptr))) { return Check(hr); }
		work->recording = true;
		*a_list = work->lists[work->slot].Get();
		return S_OK;
	}

	HRESULT Interop::Submit(Work a_work)
	{
		auto* work = Get(a_work);
		if (!Ready() || !work || !work->recording) { return E_UNEXPECTED; }
		auto hr = work->lists[work->slot]->Close();
		work->recording = false;
		if (FAILED(hr)) { return Check(hr); }
		ID3D12CommandList* lists[]{ work->lists[work->slot].Get() };
		queue_->ExecuteCommandLists(1, lists);
		hr = queue_->Signal(work->fence12.Get(), ++work->value);
		if (FAILED(hr)) { return Check(hr); }
		work->submitted[work->slot] = work->value;
		work->slot = (work->slot + 1) % kCommandSlots;
		return S_OK;
	}

	HRESULT Interop::Drain(DWORD a_timeoutMs)
	{
		if (context11_) { context11_->Flush(); }
		for (auto& work : work_) {
			if (work.fence12 && work.value) {
				const auto hr = WaitCPU(work, work.value, a_timeoutMs);
				if (FAILED(hr)) { return hr; }
			}
		}
		return S_OK;
	}

	std::uint64_t Interop::LastValue(Work a_work) const
	{
		const auto index = static_cast<std::size_t>(a_work);
		return index < work_.size() ? work_[index].value : 0;
	}

	std::size_t Interop::CurrentSlot(Work a_work) const
	{
		const auto index = static_cast<std::size_t>(a_work);
		return index < work_.size() ? work_[index].slot : kCommandSlots;
	}

	HRESULT Interop::RecordCopy(ID3D12GraphicsCommandList* a_list,
		ID3D12Resource* a_source, ID3D12Resource* a_destination)
	{
		if (!a_list || !a_source || !a_destination || a_source == a_destination) { return E_INVALIDARG; }
		const auto source = a_source->GetDesc();
		const auto destination = a_destination->GetDesc();
		if (source.Dimension != destination.Dimension || source.Width != destination.Width ||
			source.Height != destination.Height || source.Format != destination.Format ||
			source.DepthOrArraySize != destination.DepthOrArraySize || source.MipLevels != destination.MipLevels ||
			source.SampleDesc.Count != destination.SampleDesc.Count || source.SampleDesc.Quality != destination.SampleDesc.Quality) {
			return E_INVALIDARG;
		}
		D3D12_RESOURCE_BARRIER barriers[2]{};
		for (auto& barrier : barriers) {
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		}
		barriers[0].Transition.pResource = a_source;
		barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[1].Transition.pResource = a_destination;
		barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		a_list->ResourceBarrier(2, barriers);
		a_list->CopyResource(a_destination, a_source);
		for (auto& barrier : barriers) { std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); }
		a_list->ResourceBarrier(2, barriers);
		return S_OK;
	}

	void Interop::AbandonInFlightObjects()
	{
		for (auto& work : work_) {
			for (auto& list : work.lists) { (void)list.Detach(); }
			for (auto& allocator : work.allocators) { (void)allocator.Detach(); }
			(void)work.fence11.Detach();
			(void)work.fence12.Detach();
			// SetEventOnCompletion may still reference this event after timeout.
			work.event = nullptr;
		}
		(void)queue_.Detach();
		(void)context11_.Detach();
		(void)device11_.Detach();
		(void)device12_.Detach();
	}
}
