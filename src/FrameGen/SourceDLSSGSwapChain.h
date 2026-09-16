#pragma once

#include <dxgi1_5.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
#include "SourceDLSSGInterop.h"

namespace TheosRenderPipeline::SourceDLSSG {
class Backend;

// D3D11-facing transport over Streamline's native D3D12 swapchain.
class SwapChain final : public IDXGISwapChain4
{
public:
	SwapChain(
		IDXGISwapChain* a_inner,
		Backend& a_backend, DXGI_FORMAT a_gameFormat);

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_iid, void** a_object) override;
	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;

	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a_name, UINT a_size, const void* a_data) override;
	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID a_name, const IUnknown* a_unknown) override;
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a_name, UINT* a_size, void* a_data) override;
	HRESULT STDMETHODCALLTYPE GetParent(REFIID a_iid, void** a_parent) override;
	HRESULT STDMETHODCALLTYPE GetDevice(REFIID a_iid, void** a_device) override;

	HRESULT STDMETHODCALLTYPE Present(UINT a_syncInterval, UINT a_flags) override;
	HRESULT STDMETHODCALLTYPE GetBuffer(UINT a_buffer, REFIID a_iid, void** a_surface) override;
	HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL a_fullscreen, IDXGIOutput* a_target) override;
	HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* a_fullscreen, IDXGIOutput** a_target) override;
	HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* a_desc) override;
	HRESULT STDMETHODCALLTYPE ResizeBuffers(
		UINT a_bufferCount,
		UINT a_width,
		UINT a_height,
		DXGI_FORMAT a_format,
		UINT a_flags) override;
	HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* a_desc) override;
	HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** a_output) override;
	HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* a_stats) override;
	HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* a_count) override;

	HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* a_desc) override;
	HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a_desc) override;
	HRESULT STDMETHODCALLTYPE GetHwnd(HWND* a_window) override;
	HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID a_iid, void** a_window) override;
	HRESULT STDMETHODCALLTYPE Present1(
		UINT a_syncInterval,
		UINT a_flags,
		const DXGI_PRESENT_PARAMETERS* a_parameters) override;
	BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
	HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** a_output) override;
	HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* a_color) override;
	HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* a_color) override;
	HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION a_rotation) override;
	HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* a_rotation) override;

	HRESULT STDMETHODCALLTYPE SetSourceSize(UINT a_width, UINT a_height) override;
	HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* a_width, UINT* a_height) override;
	HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT a_latency) override;
	HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* a_latency) override;
	HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
	HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* a_matrix) override;
	HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* a_matrix) override;

	UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
	HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(
		DXGI_COLOR_SPACE_TYPE a_colorSpace,
		UINT* a_support) override;
	HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE a_colorSpace) override;
	HRESULT STDMETHODCALLTYPE ResizeBuffers1(
		UINT a_bufferCount,
		UINT a_width,
		UINT a_height,
		DXGI_FORMAT a_format,
		UINT a_flags,
		const UINT* a_creationNodeMask,
		IUnknown* const* a_presentQueue) override;

	HRESULT STDMETHODCALLTYPE SetHDRMetaData(
		DXGI_HDR_METADATA_TYPE a_type,
		UINT a_size,
		void* a_metadata) override;

	HRESULT RebuildBuffers();

private:
	~SwapChain();

	std::atomic<ULONG> references_{ 1 };
	Microsoft::WRL::ComPtr<IDXGISwapChain> inner_;
	Microsoft::WRL::ComPtr<IDXGISwapChain1> inner1_;
	Microsoft::WRL::ComPtr<IDXGISwapChain2> inner2_;
	Microsoft::WRL::ComPtr<IDXGISwapChain3> inner3_;
	Microsoft::WRL::ComPtr<IDXGISwapChain4> inner4_;
	Backend& backend_;
	DXGI_FORMAT gameFormat_;
	std::array<SharedTexture, 2> buffers_;
	std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> nativeBuffers_;
	HRESULT BeginPresent();
	void ObservePresentationFeedback(HRESULT a_presentResult);
};


}
