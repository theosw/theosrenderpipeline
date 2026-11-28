#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <concepts>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace TheosRenderPipeline::PluginPaths
{
	// Non-throwing UTF-8 conversion for logs and support text: path::string()
	// converts through CP_ACP on MSVC and throws on non-ANSI Windows paths.
	inline std::string ToUtf8(std::wstring_view a_wide)
	{
		if (a_wide.empty()) {
			return {};
		}
		const int size = ::WideCharToMultiByte(CP_UTF8, 0, a_wide.data(),
			static_cast<int>(a_wide.size()), nullptr, 0, nullptr, nullptr);
		if (size <= 0) {
			return {};
		}
		std::string utf8(static_cast<std::size_t>(size), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), static_cast<int>(a_wide.size()),
			utf8.data(), size, nullptr, nullptr);
		return utf8;
	}

	template <std::same_as<std::filesystem::path> Path>
	inline std::string ToUtf8(const Path& a_path)
	{
		return ToUtf8(std::wstring_view(a_path.native()));
	}

	inline std::filesystem::path FromUtf8(std::string_view a_utf8)
	{
		if (a_utf8.empty()) {
			return {};
		}
		const int size = ::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(),
			static_cast<int>(a_utf8.size()), nullptr, 0);
		if (size <= 0) {
			return {};
		}
		std::wstring wide(static_cast<std::size_t>(size), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()),
			wide.data(), size);
		return std::filesystem::path(std::move(wide));
	}

	inline std::filesystem::path Normalize(const std::filesystem::path& a_path)
	{
		std::error_code error;
		auto absolute = std::filesystem::absolute(a_path, error);
		if (error) {
			return a_path.lexically_normal();
		}
		auto canonical = std::filesystem::weakly_canonical(absolute, error);
		return error ? absolute.lexically_normal() : canonical;
	}

	namespace detail
	{
		// Raw GetModuleFileNameW query; accepts nullptr to query the current process executable.
		inline std::filesystem::path QueryModuleFileName(HMODULE a_module)
		{
			wchar_t stack[1024];
			const auto smallWritten = ::GetModuleFileNameW(a_module, stack, static_cast<DWORD>(std::size(stack)));
			if (smallWritten > 0 && smallWritten < std::size(stack)) {
				return std::filesystem::path(std::wstring_view(stack, smallWritten));
			}
			if (smallWritten == 0) {
				return {};
			}
			std::wstring path(32768, L'\0');
			const auto written = ::GetModuleFileNameW(a_module, path.data(), static_cast<DWORD>(path.size()));
			if (written == 0 || written >= path.size()) {
				return {};
			}
			path.resize(written);
			return path;
		}
	}

	// The loader's path for a module, as GetModuleFileNameW reports it, not
	// normalized. It needs no file access. Normalize opens the file, and under
	// MO2 every open goes through its virtual file system, so a walk over all
	// loaded modules (about 500 in a large modlist) must not normalize each one.
	// Returns empty for nullptr so callers passing GetModuleHandleW(...) do not
	// mistake an unloaded DLL for the host executable.
	inline std::filesystem::path RawModulePath(HMODULE a_module)
	{
		return a_module ? detail::QueryModuleFileName(a_module) : std::filesystem::path{};
	}

	inline std::filesystem::path ModulePath(HMODULE a_module)
	{
		auto path = RawModulePath(a_module);
		return path.empty() ? path : Normalize(path);
	}

	inline bool EqualNormalizedPath(const std::filesystem::path& a_left, const std::filesystem::path& a_right)
	{
		return _wcsicmp(a_left.c_str(), a_right.c_str()) == 0;
	}

	inline bool EqualPath(const std::filesystem::path& a_left, const std::filesystem::path& a_right)
	{
		return EqualNormalizedPath(Normalize(a_left), Normalize(a_right));
	}

	// A snapshot of the loaded modules, grown until it is complete.
	inline std::vector<HMODULE> LoadedModules()
	{
		std::vector<HMODULE> modules(512);
		DWORD bytes{};
		for (;;) {
			if (!::K32EnumProcessModules(::GetCurrentProcess(), modules.data(),
				static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) { return {}; }
			if (bytes <= modules.size() * sizeof(HMODULE)) { break; }
			modules.resize(bytes / sizeof(HMODULE));
		}
		modules.resize(bytes / sizeof(HMODULE));
		return modules;
	}

	// The first loaded module, retained, whose unnormalized path passes
	// `a_candidate` and whose normalized path and handle pass `a_match`.
	// `a_candidate` must not reject a module `a_match` would accept; it only
	// spares the normalization of modules that cannot match.
	template <class Candidate, class Match> HMODULE RetainLoadedModuleIf(Candidate&& a_candidate, Match&& a_match)
	{
		for (const auto module : LoadedModules()) {
			HMODULE retained{};
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
				reinterpret_cast<LPCWSTR>(module), &retained)) { continue; }
			// Compare after retaining: an enumerated address can have been reused.
			try {
				const auto raw = RawModulePath(retained);
				if (!raw.empty() && a_candidate(raw) && a_match(Normalize(raw), retained)) { return retained; }
			} catch (...) {
				::FreeLibrary(retained);
				throw;
			}
			::FreeLibrary(retained);
		}
		return nullptr;
	}

	template <class Match> HMODULE RetainLoadedModuleIf(Match&& a_match)
	{
		return RetainLoadedModuleIf([](const std::filesystem::path&) { return true; }, std::forward<Match>(a_match));
	}

	// Select an already loaded module by its configured path. The caller owns
	// the returned reference; a same-named DLL in another directory is unrelated.
	inline HMODULE RetainLoadedModule(const std::filesystem::path& a_path)
	{
		if (!a_path.is_absolute()) { return nullptr; }
		const auto path = Normalize(a_path);
		const auto requestedName = a_path.filename();
		const auto resolvedName = path.filename();
		// MO2 maps GetModuleFileNameW to the virtual Data tree, but does not
		// apply that mapping to GetModuleHandleExW's filename lookup, so the
		// modules are compared by path. Normalization keeps the file name
		// unless it resolves a link or an 8.3 short name, so only a module
		// named like the requested or resolved file, or with a short name,
		// can match.
		const auto candidate = [&](const std::filesystem::path& raw) {
			const auto name = raw.filename();
			return _wcsicmp(name.c_str(), requestedName.c_str()) == 0 ||
				_wcsicmp(name.c_str(), resolvedName.c_str()) == 0 ||
				name.native().find(L'~') != std::wstring::npos;
		};
		return RetainLoadedModuleIf(candidate, [&](const std::filesystem::path& normalized, HMODULE) {
			return _wcsicmp(normalized.c_str(), path.c_str()) == 0;
		});
	}

    // CS owns a separate runtime bundle. Only that coordinated mode admits
    // same-named modules elsewhere; our configured file must still be unowned.
    inline bool HasConflictingLoadedModule(const std::filesystem::path& path, bool allowSeparateModules)
    {
        const auto configured = RetainLoadedModule(path);
        if (configured) {
            ::FreeLibrary(configured);
            return true;
        }
        return !allowSeparateModules && ::GetModuleHandleW(path.filename().c_str()) != nullptr;
    }

    inline std::filesystem::path Directory()
    {
        // Use the game's virtual Data tree. MO2 may load our replacement DLL
        // from a different physical mod than the settings, shaders and runtimes.
        static const std::filesystem::path directory = []() -> std::filesystem::path {
            const auto executable = detail::QueryModuleFileName(nullptr);
            return executable.empty() ? std::filesystem::path{} : executable.parent_path() / L"Data" / L"SKSE" / L"Plugins";
        }();
        return directory;
    }

    inline std::filesystem::path AMDDirectory() { return Directory() / L"TheosRenderPipeline" / L"AMD"; }

    // Intel's XeSS, XeFG and XeLL runtimes.
    inline std::filesystem::path IntelDirectory() { return Directory() / L"TheosRenderPipeline" / L"Intel"; }
}
