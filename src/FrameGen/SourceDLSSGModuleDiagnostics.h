#pragma once
#include <Windows.h>
#include <filesystem>
#include <span>

namespace TheosRenderPipeline::SourceDLSSG::ModuleDiagnostics
{
	struct ExportRequirement
	{
		const char* name;
		bool expectedPresent{ true };
	};

	// Observe the result of the existing owner lookup. Never load/select a fallback
	// module or change admission, and never let a diagnostic failure abort startup.
	void Log(const std::filesystem::path& configuredPath, HMODULE retained,
		std::span<const ExportRequirement> exports) noexcept;
}
