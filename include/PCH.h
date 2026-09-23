#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <xbyak/xbyak.h>

#include <detours/Detours.h>
#include "HookDetour.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <array>
#include <filesystem>
#include <fstream>

using namespace std::literals;

namespace stl
{
	using namespace SKSE::stl;

	template <class T, std::size_t Size = 5>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		if (Size == 6) {
			T::func = *(uintptr_t*)trampoline.write_call<6>(a_src, T::thunk);
		} else {
			T::func = trampoline.write_call<Size>(a_src, T::thunk);
		}
	}

	template <class T>
	void detour_thunk(REL::RelocationID a_relId)
	{
		const auto original = TheosRenderPipeline::HookSafety::InstallEntryDetour(a_relId.address(), (uintptr_t)&T::thunk);
		if (!original) { SKSE::stl::report_and_fail("Could not preserve an existing renderer entry hook. See TheosRenderPipeline.log."); }
		*(uintptr_t*)&T::func = original;
	}
}

namespace logger = SKSE::log;

namespace util
{
	using SKSE::stl::report_and_fail;
}

#define DLLEXPORT __declspec(dllexport)

#include "Plugin.h"
