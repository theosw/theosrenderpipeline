#pragma once

#include <Windows.h>

#include <cstddef>
#include <filesystem>
#include <memory>

namespace TheosRenderPipeline::NeuralRendering
{
	class ModulePathHook
	{
	public:
		ModulePathHook();
		~ModulePathHook();

		ModulePathHook(const ModulePathHook&) = delete;
		ModulePathHook& operator=(const ModulePathHook&) = delete;
		ModulePathHook(ModulePathHook&&) = delete;
		ModulePathHook& operator=(ModulePathHook&&) = delete;

		bool Install(HMODULE a_featureModule, const std::filesystem::path& a_normalLoaderPath);
		bool Restore();

	private:
		struct State;
		std::unique_ptr<State> state_;
	};
}
