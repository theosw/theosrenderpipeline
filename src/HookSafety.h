#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>

namespace TheosRenderPipeline::HookSafety
{
    inline bool Read(std::uintptr_t address, void* data, std::size_t bytes)
    {
        SIZE_T read{};
        return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), data, bytes, &read) && read == bytes;
    }
    inline bool Executable(std::uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) { return false; }
        return (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }
    inline bool Bytes(std::uintptr_t address, std::span<const std::uint8_t> expected)
    {
        std::array<std::uint8_t, 32> actual{};
        return !expected.empty() && expected.size() <= actual.size() &&
            Read(address, actual.data(), expected.size()) &&
            std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
    }
    inline std::uintptr_t RelativeTarget(std::uintptr_t site, std::size_t length, const void* displacement)
    {
        std::int32_t relative{};
        std::memcpy(&relative, displacement, sizeof(relative));
        return site + length + static_cast<std::intptr_t>(relative);
    }
    inline std::uintptr_t DirectCallTarget(std::uintptr_t site)
    {
        std::array<std::uint8_t, 5> bytes{};
        if (!Read(site, bytes.data(), bytes.size()) || bytes[0] != 0xE8) { return 0; }
        const auto target = RelativeTarget(site, bytes.size(), bytes.data() + 1);
        return Executable(target) ? target : 0;
    }
    inline std::uintptr_t ImportCallSlot(std::uintptr_t site)
    {
        std::array<std::uint8_t, 6> bytes{};
        if (!Read(site, bytes.data(), bytes.size()) || bytes[0] != 0xFF || bytes[1] != 0x15) { return 0; }
        return RelativeTarget(site, bytes.size(), bytes.data() + 2);
    }
    inline bool OutsideImage(std::uintptr_t target, std::uintptr_t base, std::size_t size)
    {
        return target && (target < base || target - base >= size);
    }
    inline std::uintptr_t* ImportSlot(std::uintptr_t base, const char* dll, const char* name)
    {
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
            !Read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { return nullptr; }
        const auto size = nt.OptionalHeader.SizeOfImage;
        const auto inside = [size](std::uintptr_t rva, std::size_t bytes) { return rva < size && bytes <= size - rva; };
        const auto stringAt = [&](std::uintptr_t rva, auto& text) {
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (!inside(rva + i, 1) || !Read(base + rva + i, &text[i], 1)) { return false; }
                if (!text[i]) { return true; }
            }
            return false;
        };
        const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!directory.VirtualAddress || !inside(directory.VirtualAddress, directory.Size)) { return nullptr; }
        for (std::size_t offset = 0; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= directory.Size; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            IMAGE_IMPORT_DESCRIPTOR descriptor{};
            if (!Read(base + directory.VirtualAddress + offset, &descriptor, sizeof(descriptor)) || !descriptor.Name) { break; }
            std::array<char, 256> module{};
            if (!stringAt(descriptor.Name, module) || _stricmp(module.data(), dll) != 0) { continue; }
            if (!descriptor.OriginalFirstThunk || !descriptor.FirstThunk) { return nullptr; }
            for (std::size_t i = 0; ; ++i) {
                const auto names = descriptor.OriginalFirstThunk + i * sizeof(IMAGE_THUNK_DATA64);
                const auto slot = descriptor.FirstThunk + i * sizeof(std::uintptr_t);
                IMAGE_THUNK_DATA64 thunk{};
                if (!inside(names, sizeof(thunk)) || !inside(slot, sizeof(std::uintptr_t)) ||
                    !Read(base + names, &thunk, sizeof(thunk)) || !thunk.u1.AddressOfData) { break; }
                if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal)) { continue; }
                std::array<char, 256> symbol{};
                if (stringAt(thunk.u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name), symbol) && std::strcmp(symbol.data(), name) == 0) {
                    return reinterpret_cast<std::uintptr_t*>(base + slot);
                }
            }
        }
        return nullptr;
    }
    inline bool DirectCall(std::uintptr_t site, std::uintptr_t expectedTarget,
        std::uintptr_t imageBase, std::size_t imageSize)
    {
        const auto target = DirectCallTarget(site);
        // Preserve an earlier plugin's executable call trampoline. A different
        // engine callee at this offset is a mismatched contract, not a chain.
        return target && (target == expectedTarget || OutsideImage(target, imageBase, imageSize));
    }
    inline std::uintptr_t EntryJumpTarget(std::uintptr_t site)
    {
        std::array<std::uint8_t, 6> bytes{};
        if (!Read(site, bytes.data(), bytes.size())) { return 0; }
        std::uintptr_t target{};
        if (bytes[0] == 0xE9) { target = RelativeTarget(site, 5, bytes.data() + 1); }
        else if (bytes[0] == 0xFF && bytes[1] == 0x25) {
            if (!Read(RelativeTarget(site, 6, bytes.data() + 2), &target, sizeof(target))) { return 0; }
        }
        return target != site && Executable(target) ? target : 0;
    }
    inline bool Entry(std::uintptr_t site, std::span<const std::uint8_t> expected,
        std::uintptr_t imageBase, std::size_t imageSize)
    {
        return Bytes(site, expected) || OutsideImage(EntryJumpTarget(site), imageBase, imageSize);
    }
    inline std::uintptr_t ImportCallTarget(std::uintptr_t site)
    {
        std::uintptr_t target{};
        return Read(ImportCallSlot(site), &target, sizeof(target)) && Executable(target) ? target : 0;
    }
    inline bool ImportCall(std::uintptr_t site, std::uintptr_t namedSlot,
        std::uintptr_t imageBase, std::size_t imageSize)
    {
        const auto slot = ImportCallSlot(site);
        const auto target = ImportCallTarget(site);
        std::uintptr_t namedTarget{};
        // Display Tweaks replaces this FF15 call's slot with its own trampoline.
        // Preserve the callable predecessor while still rejecting other engine
        // callees, unreadable slots and changed instruction forms.
        // The named import is hooked separately, even when this call uses a
        // replacement slot. Preflight it too, before either site is changed.
        return Read(namedSlot, &namedTarget, sizeof(namedTarget)) && Executable(namedTarget) &&
            target && (slot == namedSlot || OutsideImage(target, imageBase, imageSize));
    }

    // One host owns the game's device. Reject a second or reentrant creation
    // before it can replace live host resources or reinstall class-wide hooks.
    class DeviceAdmission
    {
    public:
        bool Begin() { bool expected = false; return claimed_.compare_exchange_strong(expected, true); }
    private:
        std::atomic_bool claimed_{};
    };

    class SlotRegistry
    {
    public:
        template<class Original> bool Install(std::uintptr_t* slot, std::uintptr_t replacement, Original& original)
        {
            static_assert(sizeof(Original) == sizeof(std::uintptr_t));
            std::scoped_lock lock(mutex_);
            std::uintptr_t saved{}, current{};
            std::memcpy(&saved, &original, sizeof(saved));
            if (!slot || !Executable(replacement) || !Read(reinterpret_cast<std::uintptr_t>(slot), &current, sizeof(current))) { return false; }
            for (std::size_t i = 0; i < used_; ++i) {
                const auto& record = records_[i];
                if (record.slot == slot) {
                    // A later plugin may now head the chain. Never splice our
                    // hook into that chain again or replace its saved original.
                    return record.replacement == replacement && saved == record.original && Executable(current);
                }
            }
            if (!Executable(current) || current == replacement || (saved && saved != current) || used_ == records_.size()) { return false; }
            DWORD previous{};
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &previous)) { return false; }
            // Publish the original before another thread can enter our hook.
            if (saved != current) { std::memcpy(&original, &current, sizeof(current)); }
            const auto exchanged = reinterpret_cast<std::uintptr_t>(InterlockedCompareExchangePointer(
                reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(replacement), reinterpret_cast<void*>(current)));
            DWORD ignored{};
            const bool restored = VirtualProtect(slot, sizeof(*slot), previous, &ignored) != FALSE;
            if (exchanged != current) {
                if (saved != current) { std::memcpy(&original, &saved, sizeof(saved)); }
                return false;
            }
            records_[used_++] = {slot, replacement, current};
            return restored;
        }
    private:
        struct Record { std::uintptr_t* slot{}; std::uintptr_t replacement{}, original{}; };
        std::mutex mutex_;
        std::array<Record, 64> records_{};
        std::size_t used_{};
    };
}
