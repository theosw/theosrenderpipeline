#pragma once

#include <SolFGTextureProviderAPI.h>

#include <array>
#include <cstdint>
#include <string>

class TextureProviderBridge
{
public:
	struct Settings
	{
		bool enabled{ false };
		std::array<std::uint32_t, SolFGTextureProviderAPI::kCategoryCount> maxSize{
			2048, 2048, 2048, 2048, 2048, 2048
		};

		bool operator==(const Settings&) const = default;
	};

	struct Telemetry
	{
		bool hooksInstalled{ false };
		std::uint64_t reducedTextures{ 0 };
		std::uint64_t estimatedBytesAvoided{ 0 };
		std::uint64_t namesResolved{ 0 };
		std::uint64_t nameLookups{ 0 };
	};

	static TextureProviderBridge* GetSingleton()
	{
		static TextureProviderBridge bridge;
		return &bridge;
	}

	bool Read(Settings& a_settings, Telemetry* a_telemetry = nullptr);
	bool Apply(const Settings& a_settings, bool a_save);
	const char* Status() const { return status_.c_str(); }

private:
	bool Resolve();

	const SolFGTextureProviderAPI::ProviderV1* provider_{ nullptr };
	std::string status_{ "TextureDownscaler.dll not loaded" };
};
