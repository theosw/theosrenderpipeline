#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace TheosRenderPipeline::SourceDLSSG::MFGContract
{
	// Adapted from RTX40MFG-Unlock patcher.cpp, MIT, Michael Robles 2026.
	// Wrapper profiles and the two-byte provider branch follow upstream v1.3.3.
	// Locate instructions by context; offsets and DLL versions are not admission checks.
	inline constexpr std::array<std::uint8_t, 10> wrapperPattern{ 0xBA, 5, 0, 0, 0, 0x3B, 0xCA, 0x0F, 0x42, 0xD1 };
	inline constexpr std::array<std::uint8_t, 13> providerPattern{ 0x84, 0xD2, 0x0F, 0x84, 3, 1, 0, 0, 0xBE, 5, 0, 0, 0 };
	inline constexpr std::array<std::uint8_t, 2> providerOriginal{ 0x0F, 0x84 };
	// Skip the old near-branch displacement with one aligned two-byte update.
	inline constexpr std::array<std::uint8_t, 2> providerReplacement{ 0xEB, 0x04 };
	inline bool MatchesWrapper(std::span<const std::uint8_t> bytes)
	{
		return bytes.size() >= wrapperPattern.size() && bytes[0] == 0xBA &&
			(bytes[1] == 1 || bytes[1] == 3 || bytes[1] == 5) &&
			std::equal(wrapperPattern.begin() + 2, wrapperPattern.end(), bytes.begin() + 2);
	}
	inline bool MatchesProvider(std::span<const std::uint8_t> bytes)
	{
		return bytes.size() >= providerPattern.size() &&
			std::equal(providerPattern.begin(), providerPattern.end(), bytes.begin());
	}
	inline constexpr std::size_t notFound = static_cast<std::size_t>(-1);
	inline std::size_t FindUnique(std::span<const std::uint8_t> bytes, std::span<const std::uint8_t> pattern)
	{
		if (pattern.empty() || bytes.size() < pattern.size()) { return notFound; }
		auto found = notFound;
		for (std::size_t i = 0; i <= bytes.size() - pattern.size(); ++i) {
			if (!std::equal(pattern.begin(), pattern.end(), bytes.begin() + i)) { continue; }
			if (found != notFound) { return notFound; }
			found = i;
		}
		return found;
	}
	// An opted-in but incomplete patch must not trust the now-raised runtime cap.
	// Keep internal capacity at five generated frames and vary only
	// the per-real-frame request once capability, temporal and wrapper binding
	// are ready. Never infer readiness from the raised runtime maximum alone.
	inline constexpr std::uint32_t kFixedGeneratedCapacity = 5;
	constexpr std::uint32_t Maximum(bool requested, bool ready, std::uint32_t runtimeMaximum)
	{
		return requested ? (std::min)(runtimeMaximum, ready ? kFixedGeneratedCapacity : 1u) : runtimeMaximum;
	}
	// Native hardware can use the runtime capability directly. The Ada unlock
	// may use it only after every patch and wrapper-identity gate is complete.
	// Never synthesize dynamic support when the live runtime reports it false.
	constexpr bool Dynamic(bool unlockRequested, bool unlockReady, bool runtimeSupported)
	{
		return runtimeSupported && (!unlockRequested || unlockReady);
	}
	// Pinned upstream failure codes: failed publication or restart-required
	// cannot safely be treated like a pre-publication identity rejection.
	constexpr bool PublicationUncertain(std::uint32_t failure) { return failure == 10 || failure == 11; }
}
