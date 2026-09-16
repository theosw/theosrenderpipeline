#pragma once

#include <cstdint>

namespace SolFGTextureProviderAPI
{
	inline constexpr std::uint32_t kVersion1 = 1;
	inline constexpr std::uint32_t kCategoryCount = 6;

	struct SettingsV1
	{
		std::uint32_t structSize{ sizeof(SettingsV1) };
		std::uint32_t apiVersion{ kVersion1 };
		std::int32_t enabled{ 0 };
		std::uint32_t maxSize[kCategoryCount]{};
	};

	struct TelemetryV1
	{
		std::uint32_t structSize{ sizeof(TelemetryV1) };
		std::uint32_t apiVersion{ kVersion1 };
		std::int32_t hooksInstalled{ 0 };
		std::uint32_t reserved{ 0 };
		std::uint64_t reducedTextures{ 0 };
		std::uint64_t estimatedBytesAvoided{ 0 };
		std::uint64_t namesResolved{ 0 };
		std::uint64_t nameLookups{ 0 };
	};

	using GetSettingsFn = std::int32_t(__cdecl*)(SettingsV1*);
	using ApplySettingsFn = std::int32_t(__cdecl*)(const SettingsV1*);
	using SaveSettingsFn = std::int32_t(__cdecl*)();
	using ReloadSettingsFn = std::int32_t(__cdecl*)();
	using GetTelemetryFn = std::int32_t(__cdecl*)(TelemetryV1*);

	struct ProviderV1
	{
		std::uint32_t structSize;
		std::uint32_t apiVersion;
		GetSettingsFn getSettings;
		ApplySettingsFn applySettings;
		SaveSettingsFn saveSettings;
		ReloadSettingsFn reloadSettings;
		GetTelemetryFn getTelemetry;
	};

	using GetProviderFn = const ProviderV1*(__cdecl*)(std::uint32_t);
}
