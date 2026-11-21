#pragma once
#include <Windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

// Every GPU Windows reports, for the startup log. A second GPU (for example a
// Ryzen iGPU next to a Radeon card) is invisible in the rendering-adapter line,
// and some HIP/ZLUDA problems have depended on it.
namespace TheosRenderPipeline::AdapterInventory
{
	struct Entry
	{
		std::string name;
		std::uint32_t vendor{}, device{}, revision{};
		LUID luid{};
		std::uint64_t dedicatedMiB{}, sharedMiB{};
		bool software{}, rendering{};
	};

	inline std::vector<Entry> List(LUID rendering)
	{
		std::vector<Entry> out;
		Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { return out; }
		Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
		for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
			DXGI_ADAPTER_DESC1 desc{};
			if (FAILED(adapter->GetDesc1(&desc))) { continue; }
			Entry e;
			e.name = std::filesystem::path(desc.Description).string();
			e.vendor = desc.VendorId; e.device = desc.DeviceId; e.revision = desc.Revision;
			e.luid = desc.AdapterLuid;
			e.dedicatedMiB = desc.DedicatedVideoMemory >> 20; e.sharedMiB = desc.SharedSystemMemory >> 20;
			e.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
			e.rendering = desc.AdapterLuid.HighPart == rendering.HighPart && desc.AdapterLuid.LowPart == rendering.LowPart;
			out.push_back(std::move(e));
		}
		return out;
	}

	inline std::string Format(std::size_t index, const Entry& e)
	{
		return std::format("adapter {}: {} vendor=0x{:04X} device=0x{:04X} rev=0x{:02X} LUID={:08X}:{:08X} dedicated={} MiB shared={} MiB{}{}",
			index, e.name, e.vendor, e.device, e.revision, static_cast<std::uint32_t>(e.luid.HighPart), e.luid.LowPart,
			e.dedicatedMiB, e.sharedMiB, e.software ? " software" : "", e.rendering ? " (rendering)" : "");
	}

	// Hardware GPUs other than the rendering one: a second card or an iGPU.
	inline std::size_t OtherHardware(const std::vector<Entry>& entries)
	{
		std::size_t n = 0;
		for (const auto& e : entries) { n += !e.software && !e.rendering; }
		return n;
	}
}
