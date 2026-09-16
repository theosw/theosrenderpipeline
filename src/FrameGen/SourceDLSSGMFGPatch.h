#pragma once
#include "SourceDLSSGMFGContract.h"
#include <Windows.h>
#include <cstring>
#include <intrin.h>

namespace TheosRenderPipeline::SourceDLSSG::MFGPatch
{
	inline bool ReadBytes(void* destination, const void* source, std::size_t size)
	{
		__try { std::memcpy(destination, source, size); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	template <class Matcher>
	std::uint8_t* FindExecutable(HMODULE module, std::size_t patternSize, Matcher matches)
	{
		const auto base = reinterpret_cast<std::uint8_t*>(module);
		IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
		if (!patternSize || !base || !ReadBytes(&dos, base, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
			dos.e_lfanew <= 0 || dos.e_lfanew > 1024 * 1024 || !ReadBytes(&nt, base + dos.e_lfanew, sizeof(nt)) ||
			nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { return nullptr; }
		const std::size_t size = nt.OptionalHeader.SizeOfImage;
		const std::size_t sections = dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
		if (sections > size || nt.FileHeader.NumberOfSections > (size - sections) / sizeof(IMAGE_SECTION_HEADER)) { return nullptr; }
		std::uint8_t* found{};
		for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
			IMAGE_SECTION_HEADER section{};
			if (!ReadBytes(&section, base + sections + i * sizeof(section), sizeof(section))) { return nullptr; }
			if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) { continue; }
			if (section.VirtualAddress >= size || section.Misc.VirtualSize > size - section.VirtualAddress) { return nullptr; }
			const auto bytes = std::span(base + section.VirtualAddress, section.Misc.VirtualSize);
			// Count explicitly across sections, including duplicates within one section.
			for (std::size_t j = 0; patternSize <= bytes.size() && j <= bytes.size() - patternSize; ++j) {
				if (!matches(bytes.subspan(j, patternSize))) { continue; }
				if (found) { return nullptr; }
				found = bytes.data() + j;
			}
		}
		return found;
	}
	inline bool CompareExchange16(void* address, const void* before, const void* after)
	{
		short expected{}, replacement{};
		std::memcpy(&expected, before, sizeof(expected));
		std::memcpy(&replacement, after, sizeof(replacement));
		__try { return _InterlockedCompareExchange16(static_cast<volatile short*>(address), replacement, expected) == expected; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	inline bool WriteCode(std::uint8_t* address, std::span<const std::uint8_t> before,
		std::span<const std::uint8_t> after, bool& unsafe)
	{
		if (!address || (before.size() != 2 && before.size() != 3) || before.size() != after.size() ||
			(before.size() == 2 && (reinterpret_cast<std::uintptr_t>(address) & 1)) ||
			std::memcmp(address, before.data(), before.size()) != 0) { return false; }
		DWORD old{};
		if (!VirtualProtect(address, before.size(), PAGE_EXECUTE_READWRITE, &old)) { return false; }
		const bool written = before.size() == 2
			? CompareExchange16(address, before.data(), after.data())
			: ReadBytes(address, after.data(), after.size());
		const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, after.size()) != FALSE;
		if (!written || !flushed) { unsafe = true; }
		DWORD ignored{};
		if (!VirtualProtect(address, before.size(), old, &ignored)) {
			// Try once more, but never continue rendering with unproven protection.
			if (!VirtualProtect(address, before.size(), old, &ignored)) { unsafe = true; }
		}
		if (std::memcmp(address, after.data(), after.size()) != 0) { unsafe = true; }
		return written && flushed && !unsafe;
	}
	inline bool ProviderBranchMatches(HMODULE module, const std::uint8_t* match)
	{
		// The branch target and function-extent checks follow RTX40MFG-Unlock's
		// ngx_mfg_gate.h (MIT, Michael Robles 2026).
		if (!module || !match || (reinterpret_cast<std::uintptr_t>(match + 2) & 1)) { return false; }
		DWORD64 imageBase{};
		const auto* function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(match), &imageBase, nullptr);
		if (!function || imageBase != reinterpret_cast<DWORD64>(module)) { return false; }
		const auto rva = reinterpret_cast<DWORD64>(match) - imageBase;
		std::int32_t displacement{};
		if (rva < function->BeginAddress || rva + MFGContract::providerPattern.size() > function->EndAddress ||
			!ReadBytes(&displacement, match + 4, sizeof(displacement))) { return false; }
		const auto target = static_cast<std::int64_t>(rva) + 8 + displacement;
		constexpr std::array<std::uint8_t, 4> countOne{ 0x41, 0x83, 0xF8, 0x01 };
		std::array<std::uint8_t, countOne.size()> observed{};
		return target >= function->BeginAddress && target + countOne.size() <= function->EndAddress &&
			ReadBytes(observed.data(), reinterpret_cast<const void*>(imageBase + target), observed.size()) && observed == countOne;
	}
}
