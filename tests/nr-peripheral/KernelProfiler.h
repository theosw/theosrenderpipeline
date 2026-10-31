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
// TRP_NR_KERNEL_BARRIERS=1 serializes the split kernels; TRP_NR_KERNEL_CAPTURE copies one
// launch's buffers before and after it to a file (KernelProfiler.cpp, trp_copy.cu).
// PROFILE=2 keeps whole chains (including Ex) and times each chain. Without profiling,
// TRP_NR_PASCAL_RESEARCH=1 enables TRP_NR_LAUNCH_OVERRIDE:
// kernel:<bx>x<by>x<bz>[:<gx>x<gy>x<gz>][:dynamic-shared-bytes];...
// Use kernel:<bx>x<by>x<bz>::bytes to keep the grid. Exact kernel names, decimal integers;
// invalid or duplicate rules reject the entire specification. Plain overrides preserve
// whole chains and add no GPU commands, even when capture/barrier/params flags are set.
// Both chain APIs are covered; LaunchCubinShader has no overridable block/shared inputs.
namespace KernelProfiler
{
	struct Sample
	{
		std::string name;
		std::uint32_t grid[3]{}, block[3]{}, sharedBytes{};
		double ms{};
	};
	// Before the NR runtime is loaded. Returns whether profiling collection is enabled;
	// false can also mean plain override hooks were installed successfully.
	bool Install();
	// Statistics for the requests NR made, for checking the hook took effect.
	std::string Diagnostics();
	// Begin collecting for one frame's command list recording.
	void BeginFrame();
	// After the frame's list retired: every kernel launched since BeginFrame.
	std::vector<Sample> EndFrame(std::uint64_t timestampFrequency);
}
