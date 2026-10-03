#pragma once
#include <cstdint>

namespace TheosRenderPipeline::NeuralRendering
{
	// NGX parameter names for each DLSSNR subrect. They are invariant, so build
	// them at compile time instead of formatting 36 names per evaluation.
	struct SubrectKeys
	{
		const char* baseX;
		const char* baseY;
		const char* width;
		const char* height;
	};

	namespace Subrect
	{
#define TRP_NR_SUBRECT(name) inline constexpr SubrectKeys name{ "DLSSNR." #name "SubrectBaseX", \
	"DLSSNR." #name "SubrectBaseY", "DLSSNR." #name "SubrectWidth", "DLSSNR." #name "SubrectHeight" }
		TRP_NR_SUBRECT(Color);
		TRP_NR_SUBRECT(MVec);
		TRP_NR_SUBRECT(Depth);
		TRP_NR_SUBRECT(Output);
		TRP_NR_SUBRECT(Backbuffer);
		TRP_NR_SUBRECT(ControlMask);
		TRP_NR_SUBRECT(UI);
		TRP_NR_SUBRECT(UIAlpha);
		TRP_NR_SUBRECT(BidirectionalDistortionField);
#undef TRP_NR_SUBRECT
	}

	// Keep every write: the vendor parameter object may be reused or changed by
	// evaluation. Only construction of the invariant names is removed.
	template <class Parameters>
	void SetSubrect(Parameters* a_parameters, const SubrectKeys& a_keys, std::uint32_t a_width, std::uint32_t a_height)
	{
		a_parameters->Set(a_keys.baseX, 0u);
		a_parameters->Set(a_keys.baseY, 0u);
		a_parameters->Set(a_keys.width, a_width);
		a_parameters->Set(a_keys.height, a_height);
	}
}
