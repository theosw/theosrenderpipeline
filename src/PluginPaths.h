#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>

namespace TheosRenderPipeline::PluginPaths
{
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

	inline std::filesystem::path ModulePath(HMODULE a_module)
	{
		if (!a_module) {
			return {};
		}
		std::wstring path(32768, L'\0');
		const auto written = ::GetModuleFileNameW(a_module, path.data(), static_cast<DWORD>(path.size()));
		if (written == 0 || written >= path.size()) {
			return {};
		}
		path.resize(written);
		return Normalize(path);
	}

	inline bool EqualPath(const std::filesystem::path& a_left, const std::filesystem::path& a_right)
	{
		return _wcsicmp(Normalize(a_left).c_str(), Normalize(a_right).c_str()) == 0;
	}

    inline std::filesystem::path Directory()
    {
        // Use the game's virtual Data tree. MO2 may load our replacement DLL
        // from a different physical mod than the settings, shaders and runtimes.
        std::wstring executable(32768, L'\0');
        const auto size = ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (size == 0 || size >= executable.size()) { return {}; }
        executable.resize(size);
        return std::filesystem::path(executable).parent_path() / L"Data" / L"SKSE" / L"Plugins";
    }
}
