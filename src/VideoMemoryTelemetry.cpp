#include "VideoMemoryTelemetry.h"

#include <PCH.h>

namespace
{
	constexpr std::uint64_t kSampleIntervalMs = 250;
}

void VideoMemoryTelemetry::Init(ID3D11Device* a_device)
{
	adapter_.Reset();
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
}
