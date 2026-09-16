#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>

namespace TheosRenderPipeline::Compatibility
{
struct ModuleRange
{
	std::uintptr_t base{ 0 };
	std::uintptr_t end{ 0 };

	[[nodiscard]] bool Contains(const std::uintptr_t a_address, const std::size_t a_size) const noexcept
	{
		return a_address >= base && a_address <= end && a_size <= end - a_address;
	}

	[[nodiscard]] bool Contains(const std::uintptr_t a_address) const noexcept
	{
		return a_address >= base && a_address < end;
	}
};

inline std::optional<std::filesystem::path> GetModulePath(const HMODULE a_module)
{
	std::wstring buffer(32768, L'\0');
	const auto length = ::GetModuleFileNameW(a_module, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0 || length >= buffer.size()) {
		return std::nullopt;
	}
	buffer.resize(length);
	return std::filesystem::path(buffer);
}

inline std::string HashText(const std::span<const std::uint8_t> a_hash)
{
	std::string result;
	result.reserve(a_hash.size() * 2);
	for (const auto value : a_hash) {
		result += std::format("{:02X}", value);
	}
	return result;
}

inline std::optional<ModuleRange> GetModuleRange(const HMODULE a_module)
{
	const auto base = reinterpret_cast<std::uintptr_t>(a_module);
	if (!base) {
		return std::nullopt;
	}
	const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return std::nullopt;
	}
	const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0) {
		return std::nullopt;
	}
	return ModuleRange{ base, base + nt->OptionalHeader.SizeOfImage };
}
}
