#include "NeuralRuntimeKernels.h"

namespace TheosRenderPipeline::NeuralRendering
{
	bool RuntimeKernelsSupported() { return false; }

	RuntimeKernelResult ApplyRuntimeKernels(HMODULE, const std::filesystem::path&, bool, bool)
	{
		return { false, false, "NR 310.9.16 on RTX 20/30/40 needs the Universal edition" };
	}
}
