#include "PluginPaths.h"
#include <detours/Detours.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Paths = TheosRenderPipeline::PluginPaths;

void Require(bool condition, const char* message)
{
	if (!condition) { throw std::runtime_error(message); }
}

struct Module
{
	HMODULE handle{};
	~Module() { if (handle) { FreeLibrary(handle); } }
	void Release() { if (handle) { FreeLibrary(handle); handle = nullptr; } }
};

// Model MO2's reverse path mapping while leaving Windows' module lookup real.
HMODULE mappedA{}, mappedB{};
std::filesystem::path virtualA, virtualB;
decltype(&GetModuleFileNameW) originalModuleFileName{};
DWORD WINAPI VirtualModuleFileName(HMODULE module, LPWSTR output, DWORD size)
{
	const auto* path = module == mappedA ? &virtualA : module == mappedB ? &virtualB : nullptr;
	if (!path) { return originalModuleFileName(module, output, size); }
	if (size <= path->native().size()) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
	std::copy(path->native().begin(), path->native().end(), output);
	output[path->native().size()] = L'\0';
	return static_cast<DWORD>(path->native().size());
}

struct VirtualPathMapping
{
	uintptr_t previous{};
	void Install()
	{
		previous = Detours::IATHook(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),
			"kernel32.dll", "GetModuleFileNameW", reinterpret_cast<uintptr_t>(VirtualModuleFileName));
		Require(previous != 0, "virtual path mapping installed");
		originalModuleFileName = reinterpret_cast<decltype(originalModuleFileName)>(previous);
	}
	~VirtualPathMapping()
	{
		if (previous) { Detours::IATHook(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),
			"kernel32.dll", "GetModuleFileNameW", previous); }
	}
};

// Count canonicalisations: each opens the file, which MO2's virtual file system
// makes slow, so a lookup must not normalize every loaded module.
decltype(&GetFinalPathNameByHandleW) originalFinalPath{};
int finalPathCalls = 0;
DWORD WINAPI CountedFinalPath(HANDLE file, LPWSTR output, DWORD size, DWORD flags)
{
	++finalPathCalls;
	return originalFinalPath(file, output, size, flags);
}

struct FinalPathCounter
{
	std::vector<std::pair<uintptr_t, uintptr_t>> hooks;
	bool Install()
	{
		// std::filesystem's handle query can be in the executable or in the STL DLL.
		for (const auto module : {GetModuleHandleW(nullptr), GetModuleHandleW(L"msvcp140.dll")}) {
			if (!module) { continue; }
			const auto previous = Detours::IATHook(reinterpret_cast<uintptr_t>(module), "kernel32.dll",
				"GetFinalPathNameByHandleW", reinterpret_cast<uintptr_t>(CountedFinalPath));
			if (previous) {
				originalFinalPath = reinterpret_cast<decltype(originalFinalPath)>(previous);
				hooks.emplace_back(reinterpret_cast<uintptr_t>(module), previous);
			}
		}
		return !hooks.empty();
	}
	~FinalPathCounter()
	{
		for (const auto& [module, previous] : hooks) {
			Detours::IATHook(module, "kernel32.dll", "GetFinalPathNameByHandleW", previous);
		}
	}
};

int Identity(HMODULE module)
{
	auto function = reinterpret_cast<int(*)()>(GetProcAddress(module, "ModuleIdentity"));
	Require(function != nullptr, "fixture export missing");
	return function();
}

int wmain(int argc, wchar_t** argv)
{
	try {
		Require(argc == 5, "two fixture paths, load order and path mode required");
		auto pathA = Paths::Normalize(argv[1]);
		auto pathB = Paths::Normalize(argv[2]);
		Require(pathA.filename() == pathB.filename() && pathA != pathB, "same filename in different directories required");
		Module a, b;
		if (std::wstring_view(argv[3]) == L"a-first") {
			a.handle = LoadLibraryW(pathA.c_str()); b.handle = LoadLibraryW(pathB.c_str());
		} else {
			Require(std::wstring_view(argv[3]) == L"b-first", "unknown load order");
			b.handle = LoadLibraryW(pathB.c_str()); a.handle = LoadLibraryW(pathA.c_str());
		}
		Require(a.handle && b.handle && a.handle != b.handle, "separate fixture modules must load");
		VirtualPathMapping mapping;
		if (std::wstring_view(argv[4]) == L"virtual") {
			mappedA = a.handle; mappedB = b.handle;
			pathA = virtualA = pathA.parent_path() / L"virtual-data" / pathA.filename();
			pathB = virtualB = pathB.parent_path() / L"virtual-data" / pathB.filename();
			mapping.Install();
			HMODULE direct{};
			const auto found = GetModuleHandleExW(0, pathA.c_str(), &direct);
			if (direct) { FreeLibrary(direct); }
			Require(!found, "virtual path does not exist in Windows loader namespace");
		} else { Require(std::wstring_view(argv[4]) == L"direct", "unknown path mode"); }
		Module retainedA{Paths::RetainLoadedModule(pathA)};
		Module retainedB{Paths::RetainLoadedModule(pathB)};
		Require(retainedA.handle == a.handle && retainedB.handle == b.handle, "configured module selected regardless of load order");
		Require(Identity(retainedA.handle) == 1 && Identity(retainedB.handle) == 2, "selected exports retain distinct module state");
		{
			FinalPathCounter counter;
			Require(counter.Install(), "canonicalisation counter installed");
			finalPathCalls = 0;
			// The fixtures' reported paths have the configured path's shape; a missing
			// (virtual) directory costs more queries than an existing one.
			(void)Paths::Normalize(pathA);
			const int one = finalPathCalls;
			Require(one >= 1, "counter sees one normalization");
			finalPathCalls = 0;
			Module probe{Paths::RetainLoadedModule(pathA)};
			const int lookup = finalPathCalls;
			const auto modules = Paths::LoadedModules().size();
			std::cout << "lookup canonicalisations " << lookup << " (one normalization " << one << ", "
				<< modules << " modules loaded)\n";
			Require(probe.handle == a.handle, "counted lookup selects the configured module");
			// The configured path and the two same-named fixtures; every module before.
			Require(lookup <= 3 * one, "lookup normalizes only modules named like the configured file");
			Require(modules > 8, "enough modules loaded for the bound to mean something");
		}
		Module renamed{Paths::RetainLoadedModule(pathA.parent_path() / L"OTHER.dll")};
		Require(!renamed.handle, "a different file name in the same directory never matches");
		auto upper = pathB.filename().wstring();
		std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
		Module cased{Paths::RetainLoadedModule(pathB.parent_path() / upper)};
		Require(cased.handle == b.handle, "file names compare without case");
		cased.Release();
		Require(!Paths::RetainLoadedModule(pathA.parent_path() / L"missing" / pathA.filename()), "missing path must not fall back to same-named DLL");
		Require(!Paths::RetainLoadedModule(pathA.filename()), "relative name must not select a runtime");
		Require(!Paths::RetainLoadedModule({}), "empty path must not select the executable");
		Require(Paths::RawModulePath(nullptr).empty(), "RawModulePath(nullptr) must stay empty for unloaded module handles");
		Require(Paths::ModulePath(nullptr).empty(), "ModulePath(nullptr) must stay empty for unloaded module handles");
		const auto pluginDir = Paths::Directory();
		Require(!pluginDir.empty() && pluginDir.is_absolute(), "Directory() must resolve to an absolute path");
		Require(pluginDir == Paths::RawModulePath(::GetModuleHandleW(nullptr)).parent_path() / L"Data" / L"SKSE" / L"Plugins",
			"Directory() must resolve to <exe dir>/Data/SKSE/Plugins");
		const std::filesystem::path unicodePath(L"C:\\Users\\J\u00F6rg\\\u6E38\u620F\\nvngx_dlssnr.dll");
		Require(Paths::FromUtf8(Paths::ToUtf8(unicodePath)) == unicodePath,
			"ToUtf8/FromUtf8 must round-trip non-ASCII Windows paths losslessly");
		Module normalized{Paths::RetainLoadedModule(pathB.parent_path() / L"." / pathB.filename())};
		Require(normalized.handle == b.handle, "normalized configured path");
		normalized.Release();
		a.Release();
		Require(Identity(retainedA.handle) == 1, "retained reference keeps selected DLL alive");
		Require(Identity(b.handle) == 2, "retaining other DLL leaves its peer intact");
		retainedA.Release();
		Require(!Paths::RetainLoadedModule(pathA), "lookup must not reload an unloaded fixture");
		Require(Identity(retainedB.handle) == 2, "releasing first instance does not unload second");
		b.Release(); retainedB.Release();
		Require(!Paths::RetainLoadedModule(pathB), "all fixture references released");
		std::cout << "Configured module selection and independent lifetime passed\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << e.what() << '\n';
		return 1;
	}
}
