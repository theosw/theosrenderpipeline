#include "PluginPaths.h"
#include "FrameGen/SourceDLSSGModuleDiagnostics.h"
#include <detours/Detours.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>

namespace Paths = TheosRenderPipeline::PluginPaths;
namespace Diagnostics = TheosRenderPipeline::SourceDLSSG::ModuleDiagnostics;

void Require(bool condition, const char* message)
{
	if (!condition) { throw std::runtime_error(message); }
}

std::string Diagnostic(const std::filesystem::path& path, HMODULE module)
{
	std::ostringstream output;
	const auto previous = spdlog::default_logger();
	auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
	spdlog::set_default_logger(std::make_shared<spdlog::logger>("module-diagnostic-test", sink));
	constexpr Diagnostics::ExportRequirement exports[]{
		{ "ModuleIdentity" }, { "MissingExport" }, { "ModuleIdentity", false }, { "AbsentExport", false }
	};
	SetLastError(ERROR_CANCELLED);
	Diagnostics::Log(path, module, exports);
	const auto preservedError = GetLastError();
	spdlog::set_default_logger(previous);
	Require(preservedError == ERROR_CANCELLED, "diagnostics must preserve caller Win32 error");
	return output.str();
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
		const auto unloaded = Diagnostic(pathA, nullptr);
		Require(unloaded.find("fileAccessible=true") != std::string::npos &&
			unloaded.find("retained=false") != std::string::npos &&
			unloaded.find("sameNameCandidates=0") != std::string::npos,
			"present-on-disk but unloaded DLL must be distinguished");
		Require(!GetModuleHandleW(pathA.c_str()), "diagnostics must not load the configured DLL");
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
		const auto loaded = Diagnostic(pathA, retainedA.handle);
		Require(loaded.find("reported=" + pathA.string()) != std::string::npos &&
			loaded.find("configuredMatch=true") != std::string::npos && loaded.find("diskVersion=") != std::string::npos,
			"diagnostics must identify the retained module in direct and virtual modes");
		Require(loaded.find("export=ModuleIdentity present=true expectedPresent=true passed=true win32=0") != std::string::npos &&
			loaded.find("export=MissingExport present=false expectedPresent=true passed=false win32=127") != std::string::npos &&
			loaded.find("export=ModuleIdentity present=true expectedPresent=false passed=false win32=0") != std::string::npos &&
			loaded.find("export=AbsentExport present=false expectedPresent=false passed=true") != std::string::npos,
			"diagnostics must distinguish required, missing and forbidden exports without changing admission");
		const auto missingPath = pathA.parent_path() / L"missing" / pathA.filename();
		const auto foreign = Diagnostic(missingPath, nullptr);
		Require(foreign.find("fileAccessible=false") != std::string::npos &&
			foreign.find("sameNameCandidates=2") != std::string::npos &&
			foreign.find("configuredMatch=false") != std::string::npos &&
			foreign.find("reported=" + pathA.string()) != std::string::npos &&
			foreign.find("reported=" + pathB.string()) != std::string::npos &&
			foreign.find("status=not-checked reason=module-not-retained") != std::string::npos,
			"diagnostics must report both foreign DLLs without treating them as selected modules");
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
		Require(Diagnostic(pathB, nullptr).find("sameNameCandidates=0") != std::string::npos,
			"diagnostic enumeration must release all temporary module references");
		std::cout << "Configured module selection, diagnostics and independent lifetime passed\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << e.what() << '\n';
		return 1;
	}
}
