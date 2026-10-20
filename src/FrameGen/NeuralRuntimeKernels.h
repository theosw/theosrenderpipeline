#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>

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
	constexpr bool NeedsRuntimeKernels(int capability)
	{
		return capability == 75 || capability == 86 || capability == 89;
	}

	// False in Standard, which never patches the runtime.
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
}
