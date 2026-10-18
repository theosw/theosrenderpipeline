#pragma once

#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

// Manual research tool for the vendor benchmark only: times each GPU kernel
// the NR runtime launches. NR records its kernels into the caller's command
// list through NVAPI (CreateCuFunction, LaunchCuKernelChain). With
// TRP_NR_KERNEL_PROFILE=1 the benchmark wraps those entry points, splits each
// chain and brackets every kernel with timestamps on the same list.
namespace KernelProfiler
{
	struct Sample
	{
		std::string name;
		std::uint32_t grid[3]{}, block[3]{}, sharedBytes{};
		double ms{};
	};
	// Before the NR runtime is loaded. Returns false when profiling is off.
	bool Install();
	// Statistics for the requests NR made, for checking the hook took effect.
	std::string Diagnostics();
	// Begin collecting for one frame's command list recording.
	void BeginFrame();
	// After the frame's list retired: every kernel launched since BeginFrame.
	std::vector<Sample> EndFrame(std::uint64_t timestampFrequency);
}
