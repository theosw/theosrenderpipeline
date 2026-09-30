#pragma once
#include "HDROutput.h"
#include "SourceDLSSGInterop.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <array>
#include <utility>

namespace TheosRenderPipeline::SourceDLSSG
{
	// Windows HDR state of the monitor showing a window. DXGI reports output
	// colour state as of factory creation; recreate the factory when it is no
	// longer current before querying again.
	struct DisplayHDR
	{
		bool known{};
		bool active{};
		float maxLuminance{};
		HMONITOR monitor{};
		std::array<wchar_t, 32> deviceName{}; // GDI name, for DisplayConfig lookups.
	};
	DisplayHDR QueryDisplayHDR(IDXGIFactory1* factory, HWND window);
	// Windows "SDR content brightness" for a GDI display in nits; 0 when unavailable.
	float QuerySDRWhiteNits(const wchar_t* gdiDeviceName);

	// Only producers that finish in 8-bit SDR use renderer-owned HDR. FP16/RGB10
	// producers (Community Shaders HDR) keep their existing presentation path.
	constexpr bool HDROutputEligibleFormat(DXGI_FORMAT gameFormat)
	{
		return gameFormat == DXGI_FORMAT_R8G8B8A8_UNORM || gameFormat == DXGI_FORMAT_B8G8R8A8_UNORM;
	}

	// GPU time of the output pass on the presenting queue, harvested only after
	// each command slot retires (no CPU wait).
	struct HDROutputTiming
	{
		std::uint64_t samples{};
		double totalUs{}, maxUs{};
		void Add(const HDROutputTiming& other)
		{
			samples += other.samples; totalUs += other.totalUs; maxUs = (std::max)(maxUs, other.maxUs);
		}
		double AverageUs() const { return samples ? totalUs / static_cast<double>(samples) : 0.0; }
	};

	// Published for the overlay and logs.
	struct HDROutputState
	{
		bool requested{};      // Enabled in settings when the swapchain was created.
		bool native{};         // Native swapchain allocated as HDR10-capable RGB10A2.
		bool display{};        // Windows reports HDR on the game's monitor; PQ is signalled.
		bool displayKnown{};
		float displayMaxNits{};
		std::uint64_t composedFrames{}, encodedFrames{};
		HDROutputTiming gpu;
		// Display re-queries create a DXGI factory and enumerate outputs on the render thread.
		std::uint64_t displayQueries{};
		double displayQueryTotalUs{}, displayQueryMaxUs{};
		float windowsSDRWhiteNits{};      // 0 when unknown.
		std::uint64_t sdrWhiteQueries{}, sdrWhiteFailures{};
		double sdrWhiteQueryMaxUs{};
		const char* reason{"not requested"};
	};

	// Presentation-time HDR10 output for SDR producers (see HDROutput.h).
	//
	// Compose writes three targets in one draw so frame generation receives the
	// same encoding as the real frame (DLSS-G section 5.1):
	//   HUD-less  = PQ(expanded scene)
	//   UI        = PQ(UI at UI brightness) premultiplied by alpha
	//   backbuffer = UI + (1 - alpha) * PQ(expanded scene behind the UI)
	// Where the composed SDR frame differs from the tagged layers (for example
	// a late native overlay), the scene behind the UI is recovered from it so
	// that content is retained. Encode handles frames without separate UI: it writes the whole
	// frame at UI brightness, or passes SDR through when HDR is not displayable.
	//
	// All resources enter and leave in COMMON. The caller retires a command slot
	// before reuse and drains before releasing or recreating targets.
	class HDROutputPass final
	{
	public:
		static constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
		// DLSS-G requires more alpha precision than RGB10A2 for the UI tag.
		static constexpr DXGI_FORMAT kUIFormat = DXGI_FORMAT_R16G16B16A16_UNORM;

		HRESULT RecordCompose(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
			const HDROutput::ShaderConstants& constants, ID3D12Resource* composite, ID3D12Resource* ui,
			ID3D12Resource* scene, ID3D12Resource* backbuffer);
		HRESULT RecordEncode(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
			const HDROutput::ShaderConstants& constants, ID3D12Resource* composite, ID3D12Resource* backbuffer);

		bool TargetsMatch(UINT width, UINT height) const;
		HRESULT CreateTargets(ID3D12Device* device, UINT width, UINT height); // Caller has drained.
		ID3D12Resource* HudlessTarget() const { return hudless_.Get(); }
		ID3D12Resource* UITarget() const { return ui_.Get(); }
		// Optional; a failure leaves the pass untimed. Frequency is the presenting queue's.
		HRESULT EnableTiming(ID3D12Device* device, std::uint64_t frequency);
		// Returns samples harvested since the previous call.
		HDROutputTiming TakeTiming() { return std::exchange(harvested_, {}); }
	private:
		void HarvestTiming(std::size_t slot);
		HRESULT Initialize(ID3D12Device* device);
		HRESULT Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
			const HDROutput::ShaderConstants& constants, ID3D12Resource* composite, ID3D12Resource* ui,
			ID3D12Resource* scene, ID3D12Resource* backbuffer, bool compose);
		Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> compose_, encode_;
		std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, kCommandSlots> srv_, rtv_;
		// Retained per slot until that slot retires.
		std::array<std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4>, kCommandSlots> retained_;
		Microsoft::WRL::ComPtr<ID3D12Resource> hudless_, ui_;
		Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamps_;
		Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadback_;
		const std::uint64_t* mappedTimestamps_{};
		std::uint64_t timestampFrequency_{};
		std::array<bool, kCommandSlots> timingPending_{};
		HDROutputTiming harvested_;
	};
}
