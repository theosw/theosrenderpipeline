#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <cwctype>
#include "NeuralLaunchRequirements.h"

namespace TheosRenderPipeline::NeuralRendering
{
	// NVIDIA's NR runtime 310.9.16 ships its kernels for RTX 50 only. Universal
	// runs it on RTX 20/30/40 with a pinned kernel pack (tools/nr-runtime):
	// community SM75/86, TRP SM89 and NVIDIA's original SM120 code/PTX. The DLL
	// on disk stays NVIDIA's signed file; a loaded copy is patched in memory
	// before NGX initializes it. Standard leaves it unmodified.
	inline constexpr std::string_view kKernelRuntimeSha256 =
		"2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F";
	inline constexpr std::string_view kKernelPackSha256 =
		"751006ADB39C8DF0A7580EE095F8E605719AAA62832D6524DE30192E4A668FD3";
	// Installed next to the runtime by Universal.
	inline constexpr std::string_view kKernelPackName = "nvngx_dlssnr_310.9.16.trpk";
	// TRP Fast: the 2026-10-24 hill-climb's c9 pack, the shipped output on RTX
	// 30/40 with faster SM86 and SM89 code. The runtime list names the pack file
	// itself; that choice loads 310.9.16 under a second name (KernelPackImage).
	inline constexpr std::string_view kFastKernelPackName = "nvngx_dlssnr_310.9.16_fast.trpk";
	inline constexpr std::string_view kFastKernelPackSha256 =
		"40030052FAFF6AE7BA56D05C342239FAD9D3C7F74835E83D760111AAF4AAE22B";
	inline constexpr std::size_t kFastKernelPackBytes = 11575592;
	// A pack chosen as the runtime: NVIDIA's 310.9.16 beside it, patched with it.
	inline bool IsKernelPackChoice(const std::filesystem::path& path)
	{
		auto extension = path.extension().wstring();
		for (auto& c : extension) { c = static_cast<wchar_t>(towlower(c)); }
		return extension == L".trpk";
	}
	inline std::filesystem::path KernelPackRuntime(const std::filesystem::path& pack)
	{
		return pack.parent_path() / L"nvngx_dlssnr_310.9.16.dll";
	}
	// NVIDIA's 310.9.16 builds its GPU modules once per loaded image and keeps them,
	// so a second pack needs a second image: a copy of the runtime beside the real
	// file (MO2's virtual Data path resolved first), made once. A hard link does not
	// work: Windows loads the same file once whatever its name. Returns the copy, or
	// an empty path and why (no space, no permission).
	std::filesystem::path KernelPackImage(const std::filesystem::path& runtime, const std::filesystem::path& pack, std::string& why);
	constexpr bool NeedsRuntimeKernels(int capability)
	{
		return capability == 75 || capability == 86 || capability == 89;
	}

	// False in Standard, including its opt-in Pascal research build; only an
	// explicit research installation may bypass Standard's normal refusal.
	bool RuntimeKernelsSupported();

	struct RuntimeKernelResult
	{
		bool applied{};
		bool unsafe{}; // Unconfirmed rollback: do not enter this runtime or fall back.
		std::string status;
	};
	// a_module: a runtime with kKernelRuntimeSha256, loaded and not yet
	// initialized. Every write is checked against NVIDIA's original bytes and
	// applied before any NGX entry. The caller serializes loading/initialization;
	// newlyLoaded must be false if the image was resident before its LoadLibrary.
	// Only a fresh module or our recorded installation is accepted. Installed
	// modules and read-only packs stay resident for the process lifetime.
	// research: an unpinned pack (TRP_NR_PASCAL_PACK) and GTX 10 admission.
	RuntimeKernelResult ApplyRuntimeKernels(HMODULE a_module, const std::filesystem::path& a_pack, bool newlyLoaded,
		bool research = false);
	LaunchRequirements RuntimeLaunchRequirements(HMODULE module);
	int RuntimeKernelBundle(HMODULE module, const void* data, std::uint32_t bytes);
}
