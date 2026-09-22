#include "SourceDLSSGModuleDiagnostics.h"
#include "../PluginPaths.h"
#include <spdlog/spdlog.h>
#include <format>
#include <string>
#include <vector>

namespace TheosRenderPipeline::SourceDLSSG::ModuleDiagnostics
{
	namespace
	{
		struct LastErrorScope
		{
			DWORD saved{ ::GetLastError() };
			~LastErrorScope() { ::SetLastError(saved); }
		};
		struct ModuleReference
		{
			HMODULE handle{};
			~ModuleReference() { if (handle) { ::FreeLibrary(handle); } }
		};

		std::filesystem::path ReportedPath(HMODULE module, DWORD& error)
		{
			std::wstring buffer(32768, L'\0');
			::SetLastError(ERROR_SUCCESS);
			const auto size = ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
			error = size == 0 ? ::GetLastError() : size >= buffer.size() ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS;
			if (error != ERROR_SUCCESS) { return {}; }
			buffer.resize(size);
			return buffer;
		}

		std::string DiskVersion(const std::filesystem::path& path, DWORD& error)
		{
			DWORD ignored{};
			::SetLastError(ERROR_SUCCESS);
			const auto size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
			if (!size) { error = ::GetLastError(); return "unavailable"; }
			std::vector<std::byte> data(size);
			if (!::GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
				error = ::GetLastError(); return "unavailable";
			}
			VS_FIXEDFILEINFO* info{};
			UINT infoSize{};
			if (!::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
				!info || infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xFEEF04BD) {
				error = ERROR_RESOURCE_DATA_NOT_FOUND; return "unavailable";
			}
			error = ERROR_SUCCESS;
			return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
				HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
		}

		void LogIdentity(HMODULE module, const std::filesystem::path& configured, const char* kind)
		{
			DWORD pathError{};
			const auto reported = ReportedPath(module, pathError);
			if (reported.empty()) {
				spdlog::info("[SourceDLSSG Modules] kind={} module={} path=unavailable win32={}",
					kind, static_cast<void*>(module), pathError);
				return;
			}
			DWORD versionError{};
			const auto version = DiskVersion(reported, versionError);
			spdlog::info("[SourceDLSSG Modules] kind={} module={} reported={} normalized={} configuredMatch={} diskVersion={} versionWin32={}",
				kind, static_cast<void*>(module), reported.string(), PluginPaths::Normalize(reported).string(),
				PluginPaths::EqualPath(reported, configured), version, versionError);
		}

		void LogCandidates(const std::filesystem::path& configured)
		{
			std::vector<HMODULE> modules(128);
			DWORD bytes{};
			for (;;) {
				if (!::K32EnumProcessModules(::GetCurrentProcess(), modules.data(),
					static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) {
					const auto error = ::GetLastError();
					spdlog::warn("[SourceDLSSG Modules] diagnostic enumeration failed win32={}", error);
					return;
				}
				if (bytes <= modules.size() * sizeof(HMODULE)) { break; }
				modules.resize(bytes / sizeof(HMODULE));
			}
			modules.resize(bytes / sizeof(HMODULE));
			unsigned candidates{}, retainFailures{}, pathFailures{};
			for (const auto module : modules) {
				ModuleReference reference;
				if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
					reinterpret_cast<LPCWSTR>(module), &reference.handle)) { ++retainFailures; continue; }
				DWORD error{};
				const auto reported = ReportedPath(reference.handle, error);
				if (reported.empty()) { ++pathFailures; continue; }
				if (_wcsicmp(reported.filename().c_str(), configured.filename().c_str()) == 0) {
					++candidates;
					LogIdentity(reference.handle, configured, "same-name-candidate");
				}
			}
			spdlog::info("[SourceDLSSG Modules] configured={} sameNameCandidates={} retainFailures={} pathFailures={}; candidates are diagnostic only",
				configured.string(), candidates, retainFailures, pathFailures);
		}
	}

	void Log(const std::filesystem::path& configuredPath, HMODULE retained,
		std::span<const ExportRequirement> exports) noexcept
	{
		LastErrorScope preserveError;
		try {
			WIN32_FILE_ATTRIBUTE_DATA file{};
			const bool accessible = ::GetFileAttributesExW(configuredPath.c_str(), GetFileExInfoStandard, &file) != FALSE;
			const auto fileError = accessible ? ERROR_SUCCESS : ::GetLastError();
			spdlog::info("[SourceDLSSG Modules] configured={} normalized={} fileAccessible={} fileWin32={} retained={}",
				configuredPath.string(), PluginPaths::Normalize(configuredPath).string(), accessible, fileError, retained != nullptr);
			if (retained) { LogIdentity(retained, configuredPath, "retained"); }
			else { LogCandidates(configuredPath); }
			for (const auto& requirement : exports) {
				if (!retained) {
					spdlog::info("[SourceDLSSG Modules] file={} export={} status=not-checked reason=module-not-retained",
						configuredPath.filename().string(), requirement.name);
					continue;
				}
				::SetLastError(ERROR_SUCCESS);
				const auto function = ::GetProcAddress(retained, requirement.name);
				const auto error = function ? ERROR_SUCCESS : ::GetLastError();
				spdlog::info("[SourceDLSSG Modules] file={} export={} present={} expectedPresent={} passed={} win32={}",
					configuredPath.filename().string(), requirement.name, function != nullptr,
					requirement.expectedPresent, (function != nullptr) == requirement.expectedPresent, error);
			}
		} catch (...) {
			try { spdlog::warn("[SourceDLSSG Modules] diagnostic collection incomplete; startup checks remain authoritative"); }
			catch (...) {}
		}
	}
}
