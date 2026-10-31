#include "NeuralRuntimeKernels.h"

namespace TheosRenderPipeline::NeuralRendering
{
	bool RuntimeKernelsSupported() { return false; }
	LaunchRequirements RuntimeLaunchRequirements(HMODULE) { return {}; }
	int RuntimeKernelBundle(HMODULE, const void*, std::uint32_t) { return -1; }

	RuntimeKernelResult ApplyRuntimeKernels(HMODULE, const std::filesystem::path&, bool, bool)
	{
		return { false, false, "NR 310.9.16 on RTX 20/30/40 needs the Universal edition" };
	}
}
