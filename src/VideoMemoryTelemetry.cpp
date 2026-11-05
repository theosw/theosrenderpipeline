#include "VideoMemoryTelemetry.h"

#include <PCH.h>

#include "GpuMemoryStatistics.h"

namespace
{
	constexpr std::uint64_t kSampleIntervalMs = 250;
}

void VideoMemoryTelemetry::Init(ID3D11Device* a_device)
{
	adapter_.Reset();
	kernelStatistics_ = false;
	snapshot_ = {};
	lastSampleTick_ = 0;

	if (!a_device) {
		status_ = "D3D11 device unavailable";
		return;
	}

	Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
	if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
		FAILED(dxgiDevice->GetAdapter(&adapter)) ||
		FAILED(adapter.As(&adapter_))) {
		status_ = "DXGI 1.4 video-memory telemetry unavailable";
		logger::warn("[VideoMemory] could not acquire IDXGIAdapter3");
		return;
	}

	DXGI_ADAPTER_DESC description{};
	if (SUCCEEDED(adapter_->GetDesc(&description))) {
		snapshot_.dedicatedCapacity = description.DedicatedVideoMemory;
		luid_ = description.AdapterLuid;
		// Integrated adapters have little or no dedicated memory; DXGI's budget
		// stays the meaningful figure there.
		kernelStatistics_ = description.DedicatedVideoMemory >= (512ull << 20);
	}
	status_ = "waiting for first DXGI memory sample";
	Update();
}

void VideoMemoryTelemetry::Update()
{
	if (!adapter_) {
		return;
	}

	const auto now = ::GetTickCount64();
	if (lastSampleTick_ != 0 && now - lastSampleTick_ < kSampleIntervalMs) {
		return;
	}
	lastSampleTick_ = now;

	DXGI_QUERY_VIDEO_MEMORY_INFO info{};
	const auto result = adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
	if (FAILED(result)) {
		snapshot_.available = false;
		status_ = "DXGI local-memory query failed";
		return;
	}

	snapshot_.available = true;
	snapshot_.currentUsage = info.CurrentUsage;
	snapshot_.budget = info.Budget;
	snapshot_.availableForReservation = info.AvailableForReservation;
	snapshot_.currentReservation = info.CurrentReservation;
	status_ = "DXGI local-memory budget available";

	const auto kernel = kernelStatistics_ ? TheosRenderPipeline::GpuMemory::Query(luid_) : std::nullopt;
	snapshot_.kernelAvailable = kernel.has_value();
	snapshot_.processKnown = kernel && kernel->processKnown;
	snapshot_.adapterLimit = kernel ? kernel->adapterLimit : 0;
	snapshot_.adapterResident = kernel ? kernel->adapterResident : 0;
	snapshot_.processCommitted = kernel ? kernel->processCommitted : 0;
	snapshot_.processDemoted = kernel ? kernel->processDemoted : 0;
	if (kernel) {
		status_ = "Whole-card GPU memory available";
	}
}
