#pragma once

#include "NeuralRenderingTuning.h"
#include <cstdint>
#include <string_view>

namespace TheosRenderPipeline::NeuralRendering
{
	// Distinct inference binaries may share the same NGX parameter contract.
	// Version310 is any other runtime reporting file version 310 or later: community
	// or newer NVIDIA builds load by version, not by an exact hash.
	enum class RuntimeBuild { Unknown, Legacy, Build14, Nexus3108, Version310 };
	inline constexpr unsigned kMinimumContractVersion = 310;
	inline constexpr std::uint64_t kRuntimeSize = 165840496ull;
	inline constexpr std::string_view kLegacyRuntimeSha256 =
		"CEB6432F6FBDF44D886014BCD47241932BF8B67439FEEF9BBDD0961436662650";
	inline constexpr std::string_view kBuild14RuntimeSha256 =
		"DCC0DC2414AEDEC4A8E084647070383BE068554042587180C20C784D4772D36F";
	inline constexpr std::string_view kNexusRuntimeSha256 =
		"8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC206";

	inline bool UsesReconstructionContract(RuntimeBuild build)
	{
		return build == RuntimeBuild::Build14 || build == RuntimeBuild::Nexus3108 || build == RuntimeBuild::Version310;
	}
	// The major number of a "major.minor.build.revision" file version; 0 if unreadable.
	inline unsigned MajorVersion(std::string_view version)
	{
		unsigned major = 0;
		bool digits = false;
		for (const char c : version) {
			if (c < '0' || c > '9') { break; }
			digits = true;
			major = major * 10 + static_cast<unsigned>(c - '0');
			if (major > 100000) { return 0; }
		}
		return digits ? major : 0;
	}
	// Known builds by exact identity; any other runtime by its file version.
	inline RuntimeBuild MatchRuntime(std::uint64_t size, std::string_view hash, std::string_view version, bool allowReconstruction)
	{
		if (size == kRuntimeSize) {
			if (hash == kLegacyRuntimeSha256) { return RuntimeBuild::Legacy; }
			if (allowReconstruction && hash == kBuild14RuntimeSha256) { return RuntimeBuild::Build14; }
			if (allowReconstruction && hash == kNexusRuntimeSha256) { return RuntimeBuild::Nexus3108; }
		}
		if (allowReconstruction && MajorVersion(version) >= kMinimumContractVersion) { return RuntimeBuild::Version310; }
		return RuntimeBuild::Unknown;
	}
	inline const char* RuntimeName(RuntimeBuild build)
	{
		if (build == RuntimeBuild::Nexus3108) { return "Nexus 310.8"; }
		if (build == RuntimeBuild::Version310) { return "310.x"; }
		return build == RuntimeBuild::Build14 ? "Build14" : build == RuntimeBuild::Legacy ? "legacy" : "unknown";
	}

	struct FeatureContract
	{
		RuntimeBuild build{ RuntimeBuild::Unknown };
		std::uint32_t width{}, height{}, guideWidth{}, guideHeight{};
		int preset{}, quality{ 2 };
		float scalingRatio{};
		bool operator==(const FeatureContract&) const = default;
		bool Valid() const
		{
			return build != RuntimeBuild::Unknown && width && height && guideWidth && guideHeight &&
				preset >= 0 && preset <= 1 && std::isfinite(scalingRatio) && scalingRatio > 0 && scalingRatio <= 1;
		}
	};
	// A saved zero means the runtime default, not a universal model identity.
	inline int RequestedNetworkPreset(int saved) { return saved == 0 ? -1 : saved; }
	inline int ResolveNetworkPreset(RuntimeBuild build, int requested)
	{
		if (build == RuntimeBuild::Unknown) { return -1; }
		return requested == -1 ? (UsesReconstructionContract(build) ? 0 : 1) : requested;
	}
	inline FeatureContract MakeFeatureContract(RuntimeBuild build, std::uint32_t width, std::uint32_t height,
		std::uint32_t guideWidth, std::uint32_t guideHeight, int requestedPreset = -1)
	{
		// Full-resolution NR uses ratio=1 with this runtime even when DLSS
		// guides are smaller. The older runtime uses the guide-width ratio.
		return { build, width, height, guideWidth, guideHeight,
			ResolveNetworkPreset(build, requestedPreset),
			2, UsesReconstructionContract(build) ? 1.0f : width ? float(guideWidth) / float(width) : 0.0f };
	}
	template<class Parameters> void WriteCreationParameters(Parameters& parameters, const FeatureContract& contract)
	{
		parameters.Set("CreationNodeMask", 1u);
		parameters.Set("VisibilityNodeMask", 1u);
		parameters.Set("DLSSNR.Width", contract.width);
		parameters.Set("DLSSNR.Height", contract.height);
		parameters.Set("DLSSNR.Hint.Render.Preset", contract.preset);
		parameters.Set("PerfQualityValue", contract.quality);
		parameters.Set("DLSSNR.ScalingRatio", contract.scalingRatio);
	}
	template<class Parameters> void WriteTuningParameters(Parameters& parameters, const Tuning& requested,
		bool reset, bool inverted, RuntimeBuild build)
	{
		const auto tuning = UsesReconstructionContract(build) ? SanitizeBuild14Tuning(requested) : SanitizeTuning(requested);
		parameters.Set("DLSSNR.Intensity", tuning.intensity);
		parameters.Set("DLSSNR.LocalToneStrength", tuning.localToneStrength);
		parameters.Set("DLSSNR.LocalStructureStrength", tuning.localStructureStrength);
		parameters.Set("DLSSNR.SkinStructureStrength", tuning.skinStructureStrength);
		if (UsesReconstructionContract(build)) {
			// NGX parameter types are part of the runtime contract.
			parameters.Set("DLSSNR.UseAutoMask", int(tuning.useAutoSkinMask));
			parameters.Set("DLSSNR.Style", static_cast<unsigned>(tuning.style));
			parameters.Set("DLSSNR.Reset", int(reset));
			parameters.Set("DLSSNR.DepthInverted", int(inverted));
			parameters.Set("DLSSNR.Enabled", 1);
			parameters.Set("DLSSNR.UICorrection", int(tuning.uiCorrection));
			parameters.Set("DLSS.Indicator.Invert.X.Axis", 0);
			parameters.Set("DLSS.Indicator.Invert.Y.Axis", 0);
		} else {
			parameters.Set("DLSSNR.UseAutoMask", tuning.useAutoSkinMask ? 1u : 0u);
			parameters.Set("DLSSNR.Style", tuning.style);
			parameters.Set("DLSSNR.Reset", reset ? 1u : 0u);
			parameters.Set("DLSSNR.DepthInverted", inverted ? 1u : 0u);
			parameters.Set("DLSSNR.Enabled", 1u);
			parameters.Set("DLSSNR.UICorrection", tuning.uiCorrection ? 1u : 0u);
			parameters.Set("DLSS.Indicator.Invert.X.Axis", 0u);
			parameters.Set("DLSS.Indicator.Invert.Y.Axis", 0u);
		}
	}
}
