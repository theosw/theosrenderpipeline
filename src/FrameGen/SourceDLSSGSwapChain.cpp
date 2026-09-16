#include "SourceDLSSGSwapChain.h"

#include "SourceDLSSGBackend.h"

namespace TheosRenderPipeline::SourceDLSSG {

SwapChain::SwapChain(
	IDXGISwapChain* a_inner,
	Backend& a_backend, DXGI_FORMAT a_gameFormat) :
	inner_(a_inner), backend_(a_backend), gameFormat_(a_gameFormat)
{
	if (inner_) {
		inner_.As(&inner1_);
		inner_.As(&inner2_);
		inner_.As(&inner3_);
		inner_.As(&inner4_);
	}
}

SwapChain::~SwapChain()
{
	if (!backend_.Quiesce()) {
		// A timed-out reader may still own these resources. Retain until exit.
		for (auto& pair : buffers_) { pair.texture11.Detach(); pair.texture12.Detach(); }
		for (auto& resource : nativeBuffers_) { resource.Detach(); }
	}
}

HRESULT STDMETHODCALLTYPE SwapChain::QueryInterface(REFIID a_iid, void** a_object)
{
	if (!a_object) {
		return E_POINTER;
	}
	*a_object = nullptr;

	const bool base = a_iid == __uuidof(IUnknown) ||
		a_iid == __uuidof(IDXGIObject) ||
		a_iid == __uuidof(IDXGIDeviceSubObject) ||
		a_iid == __uuidof(IDXGISwapChain);
	const bool supportedSwapChain = base ||
		(a_iid == __uuidof(IDXGISwapChain1) && inner1_) ||
		(a_iid == __uuidof(IDXGISwapChain2) && inner2_) ||
		(a_iid == __uuidof(IDXGISwapChain3) && inner3_) ||
		(a_iid == __uuidof(IDXGISwapChain4) && inner4_);
	if (supportedSwapChain) {
		*a_object = static_cast<IDXGISwapChain4*>(this);
		AddRef();
		return S_OK;
	}
	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE SwapChain::AddRef()
{
	return references_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE SwapChain::Release()
{
	const auto remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
	if (!remaining) {
		delete this;
	}
	return remaining;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetPrivateData(REFGUID a_name, UINT a_size, const void* a_data)
{
	return inner_->SetPrivateData(a_name, a_size, a_data);
}

HRESULT STDMETHODCALLTYPE SwapChain::SetPrivateDataInterface(REFGUID a_name, const IUnknown* a_unknown)
{
	return inner_->SetPrivateDataInterface(a_name, a_unknown);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetPrivateData(REFGUID a_name, UINT* a_size, void* a_data)
{
	return inner_->GetPrivateData(a_name, a_size, a_data);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetParent(REFIID a_iid, void** a_parent)
{
	return inner_->GetParent(a_iid, a_parent);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDevice(REFIID a_iid, void** a_device)
{
	return backend_.Device11()->QueryInterface(a_iid, a_device);
}

HRESULT STDMETHODCALLTYPE SwapChain::Present(UINT a_syncInterval, UINT a_flags)
{
	if (a_flags & DXGI_PRESENT_TEST) { return inner_->Present(a_syncInterval, a_flags); }
	const auto prepared = BeginPresent();
	if (FAILED(prepared)) { return prepared; }
	const auto presented = inner_->Present(a_syncInterval, a_flags);
	ObservePresentationFeedback(presented);
	return backend_.AfterPresent(presented);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetBuffer(UINT a_buffer, REFIID a_iid, void** a_surface)
{
	if (!a_surface) { return E_POINTER; }
	*a_surface = nullptr;
	if (a_buffer >= buffers_.size() || !buffers_[a_buffer].texture11) { return DXGI_ERROR_INVALID_CALL; }
	return buffers_[a_buffer].texture11->QueryInterface(a_iid, a_surface);
}

HRESULT STDMETHODCALLTYPE SwapChain::SetFullscreenState(BOOL a_fullscreen, IDXGIOutput* a_target)
{
	return inner_->SetFullscreenState(a_fullscreen, a_target);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFullscreenState(BOOL* a_fullscreen, IDXGIOutput** a_target)
{
	return inner_->GetFullscreenState(a_fullscreen, a_target);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* a_desc)
{
	const auto result = inner_->GetDesc(a_desc);
	if (SUCCEEDED(result)) { a_desc->BufferDesc.Format = gameFormat_; }
	return result;
}

HRESULT STDMETHODCALLTYPE SwapChain::ResizeBuffers(
	UINT /*a_bufferCount*/,
	UINT a_width,
	UINT a_height,
	DXGI_FORMAT a_format,
	UINT a_flags)
{
	if (!backend_.Quiesce()) { return E_FAIL; }
	buffers_ = {}; nativeBuffers_ = {};
	const auto gameFormat = a_format == DXGI_FORMAT_UNKNOWN ? gameFormat_ : a_format;
	const auto result = inner_->ResizeBuffers(2, a_width, a_height, PresentationFormat(gameFormat),
		(a_flags & ~0x6000u) | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
	// Old wrapper buffers are already gone. A failed native resize or partial
	// rebuild cannot leave the backend advertised as ready for rendering.
	if (!backend_.Check(result, "native ResizeBuffers")) { return result; }
	gameFormat_ = gameFormat;
	const auto rebuilt = RebuildBuffers();
	if (!backend_.Check(rebuilt, "rebuild after ResizeBuffers")) { return rebuilt; }
	return backend_.ResumeAfterResize() ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE SwapChain::ResizeTarget(const DXGI_MODE_DESC* a_desc)
{
	if (!a_desc) { return E_INVALIDARG; }
	auto desc = *a_desc; desc.Format = PresentationFormat(desc.Format);
	return inner_->ResizeTarget(&desc);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetContainingOutput(IDXGIOutput** a_output)
{
	return inner_->GetContainingOutput(a_output);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* a_stats)
{
	return inner_->GetFrameStatistics(a_stats);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetLastPresentCount(UINT* a_count)
{
	return inner_->GetLastPresentCount(a_count);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* a_desc)
{
	const auto result = inner1_ ? inner1_->GetDesc1(a_desc) : E_NOINTERFACE;
	if (SUCCEEDED(result)) { a_desc->Format = gameFormat_; }
	return result;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a_desc)
{
	return inner1_ ? inner1_->GetFullscreenDesc(a_desc) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetHwnd(HWND* a_window)
{
	return inner1_ ? inner1_->GetHwnd(a_window) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetCoreWindow(REFIID a_iid, void** a_window)
{
	return inner1_ ? inner1_->GetCoreWindow(a_iid, a_window) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::Present1(
	UINT a_syncInterval,
	UINT a_flags,
	const DXGI_PRESENT_PARAMETERS* a_parameters)
{
	if (!inner1_) { return E_NOINTERFACE; }
	if (a_flags & DXGI_PRESENT_TEST) { return inner1_->Present1(a_syncInterval, a_flags, a_parameters); }
	const auto prepared = BeginPresent();
	if (FAILED(prepared)) { return prepared; }
	const auto presented = inner1_->Present1(a_syncInterval, a_flags, a_parameters);
	ObservePresentationFeedback(presented);
	return backend_.AfterPresent(presented);
}

BOOL STDMETHODCALLTYPE SwapChain::IsTemporaryMonoSupported()
{
	return inner1_ ? inner1_->IsTemporaryMonoSupported() : FALSE;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetRestrictToOutput(IDXGIOutput** a_output)
{
	return inner1_ ? inner1_->GetRestrictToOutput(a_output) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetBackgroundColor(const DXGI_RGBA* a_color)
{
	return inner1_ ? inner1_->SetBackgroundColor(a_color) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetBackgroundColor(DXGI_RGBA* a_color)
{
	return inner1_ ? inner1_->GetBackgroundColor(a_color) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetRotation(DXGI_MODE_ROTATION a_rotation)
{
	return inner1_ ? inner1_->SetRotation(a_rotation) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetRotation(DXGI_MODE_ROTATION* a_rotation)
{
	return inner1_ ? inner1_->GetRotation(a_rotation) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetSourceSize(UINT a_width, UINT a_height)
{
	return inner2_ ? inner2_->SetSourceSize(a_width, a_height) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetSourceSize(UINT* a_width, UINT* a_height)
{
	return inner2_ ? inner2_->GetSourceSize(a_width, a_height) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetMaximumFrameLatency(UINT a_latency)
{
	return inner2_ ? inner2_->SetMaximumFrameLatency(a_latency) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetMaximumFrameLatency(UINT* a_latency)
{
	return inner2_ ? inner2_->GetMaximumFrameLatency(a_latency) : E_NOINTERFACE;
}

HANDLE STDMETHODCALLTYPE SwapChain::GetFrameLatencyWaitableObject()
{
	return inner2_ ? inner2_->GetFrameLatencyWaitableObject() : nullptr;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* a_matrix)
{
	return inner2_ ? inner2_->SetMatrixTransform(a_matrix) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* a_matrix)
{
	return inner2_ ? inner2_->GetMatrixTransform(a_matrix) : E_NOINTERFACE;
}

UINT STDMETHODCALLTYPE SwapChain::GetCurrentBackBufferIndex()
{
	return inner3_ ? inner3_->GetCurrentBackBufferIndex() : 0;
}

HRESULT STDMETHODCALLTYPE SwapChain::CheckColorSpaceSupport(
	DXGI_COLOR_SPACE_TYPE a_colorSpace,
	UINT* a_support)
{
	return inner3_ ? inner3_->CheckColorSpaceSupport(PresentationColorSpace(gameFormat_, a_colorSpace), a_support) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE a_colorSpace)
{
	return inner3_ ? inner3_->SetColorSpace1(PresentationColorSpace(gameFormat_, a_colorSpace)) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::ResizeBuffers1(
	UINT /*a_bufferCount*/,
	UINT a_width,
	UINT a_height,
	DXGI_FORMAT a_format,
	UINT a_flags,
	const UINT* /*a_creationNodeMask*/,
	IUnknown* const* /*a_presentQueue*/)
{
	if (!inner3_) { return E_NOINTERFACE; }
	if (!backend_.Quiesce()) { return E_FAIL; }
	buffers_ = {}; nativeBuffers_ = {};
	const auto gameFormat = a_format == DXGI_FORMAT_UNKNOWN ? gameFormat_ : a_format;
	// This D3D11 facade owns one D3D12 device/queue, not AFR queue rotation.
	// Preserve that queue through ResizeBuffers. The pinned Streamline
	// ResizeBuffers1 path copies only the two application queue entries before
	// DLSS-G expands the native count to six, causing DXGI to overread that copy.
	// Supplying a larger caller array cannot repair its internal two-entry copy.
	const auto result = inner_->ResizeBuffers(2, a_width, a_height, PresentationFormat(gameFormat),
		(a_flags & ~0x6000u) | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
	if (!backend_.Check(result, "native ResizeBuffers1")) { return result; }
	gameFormat_ = gameFormat;
	const auto rebuilt = RebuildBuffers();
	if (!backend_.Check(rebuilt, "rebuild after ResizeBuffers1")) { return rebuilt; }
	return backend_.ResumeAfterResize() ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetHDRMetaData(
	DXGI_HDR_METADATA_TYPE a_type,
	UINT a_size,
	void* a_metadata)
{
	return inner4_ ? inner4_->SetHDRMetaData(a_type, a_size, a_metadata) : E_NOINTERFACE;
}


HRESULT SwapChain::RebuildBuffers()
{
	if (!inner3_) { return E_NOINTERFACE; }
	DXGI_SWAP_CHAIN_DESC desc{};
	auto result = inner_->GetDesc(&desc);
	if (FAILED(result)) { return result; }
	if (desc.BufferCount != buffers_.size()) { return DXGI_ERROR_INVALID_CALL; }
	for (UINT i = 0; i < buffers_.size(); ++i) {
		result = inner_->GetBuffer(i, IID_PPV_ARGS(nativeBuffers_[i].ReleaseAndGetAddressOf()));
		if (FAILED(result)) { return result; }
		D3D11_TEXTURE2D_DESC shared{};
		shared.Width = desc.BufferDesc.Width; shared.Height = desc.BufferDesc.Height;
		shared.MipLevels = 1; shared.ArraySize = 1; shared.Format = gameFormat_;
		shared.SampleDesc.Count = 1; shared.Usage = D3D11_USAGE_DEFAULT;
		shared.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		result = backend_.Transport().CreateSharedTexture(shared, buffers_[i]);
		if (FAILED(result)) { return result; }
	}
	return S_OK;
}
HRESULT SwapChain::BeginPresent()
{
	const auto index = GetCurrentBackBufferIndex();
	if (index >= buffers_.size() || !buffers_[index].texture12 || !nativeBuffers_[index]) {
		return DXGI_ERROR_INVALID_CALL;
	}
	return backend_.BeforePresent(buffers_[index].texture12.Get(), nativeBuffers_[index].Get());
}

void SwapChain::ObservePresentationFeedback(HRESULT a_presentResult)
{
	if (FAILED(a_presentResult)) { return; }
	LARGE_INTEGER observed{};
	UINT count{};
	if (!QueryPerformanceCounter(&observed) || FAILED(inner_->GetLastPresentCount(&count))) {
		backend_.RecordPresentationFeedback(0, 0, 0);
		return;
	}

	std::int64_t syncQpc{};
	DXGI_FRAME_STATISTICS statistics{};
	if (SUCCEEDED(inner_->GetFrameStatistics(&statistics)) && statistics.PresentCount == count) {
		syncQpc = statistics.SyncQPCTime.QuadPart;
	}
	backend_.RecordPresentationFeedback(count, observed.QuadPart, syncQpc);
}
}
