#pragma once

#include "../PluginPaths.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace TheosRenderPipeline::NeuralRenderingRuntimeIdentity
{
	inline constexpr std::uint64_t kExpectedRuntimeSize = 165840496ull;
	inline constexpr std::string_view kExpectedRuntimeSha256 =
		"CEB6432F6FBDF44D886014BCD47241932BF8B67439FEEF9BBDD0961436662650";

	struct Snapshot
	{
		bool present{ false };
		bool matched{ false };
		std::uint64_t size{ 0 };
		std::string version{ "not queried" };
		std::string sha256{ "not calculated" };
		std::filesystem::path path{};
	};

	using PluginPaths::Normalize;
	using PluginPaths::ModulePath;
	using PluginPaths::EqualPath;
	Snapshot VerifyExpected(
		const std::filesystem::path& a_path,
		std::uint64_t a_expectedSize,
		std::string_view a_expectedSha256);
	Snapshot Verify(const std::filesystem::path& a_path);
	// Source FeatureSession may additionally admit the known reconstruction runtimes.
	// Verify() remains legacy-only for the old bridge and diagnostic paths.
	Snapshot VerifySource(const std::filesystem::path& a_path, bool a_allowReconstruction);
}
