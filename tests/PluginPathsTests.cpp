#include "PluginPaths.h"
#include <detours/Detours.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

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
		Require(!Paths::RetainLoadedModule(pathA.parent_path() / L"missing" / pathA.filename()), "missing path must not fall back to same-named DLL");
		Require(!Paths::RetainLoadedModule(pathA.filename()), "relative name must not select a runtime");
		Require(!Paths::RetainLoadedModule({}), "empty path must not select the executable");
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
