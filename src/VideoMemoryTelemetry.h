#pragma once

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

class VideoMemoryTelemetry
{
public:
	struct Snapshot
	{
		bool available{ false };
		std::uint64_t dedicatedCapacity{ 0 };
		std::uint64_t currentUsage{ 0 };
		std::uint64_t budget{ 0 };
		std::uint64_t availableForReservation{ 0 };
		std::uint64_t currentReservation{ 0 };
	};

	static VideoMemoryTelemetry* GetSingleton()
	{
		static VideoMemoryTelemetry telemetry;
		return &telemetry;
	}

	void Init(ID3D11Device* a_device);
	void Update();
	const Snapshot& GetSnapshot() const { return snapshot_; }
	const char* Status() const { return status_.c_str(); }

private:
	Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
	Snapshot snapshot_{};
	std::string status_{ "D3D11 adapter unavailable" };
	std::uint64_t lastSampleTick_{ 0 };
};
