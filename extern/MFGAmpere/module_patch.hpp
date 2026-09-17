#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace trp::ampere::memory {
inline bool Copy(void* to, const void* from, std::size_t bytes) noexcept {
    __try { std::memcpy(to, from, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Read(const void* from, T& value) noexcept { return Copy(&value, from, sizeof(value)); }
inline bool Equal(const void* address, std::span<const std::uint8_t> value) noexcept {
    __try { return std::memcmp(address, value.data(), value.size()) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Image {
    std::uint8_t* base{};
    std::size_t size{};
    IMAGE_NT_HEADERS64 nt{};
    std::vector<IMAGE_SECTION_HEADER> sections;
    bool Contains(std::size_t rva, std::size_t bytes) const { return rva <= size && bytes <= size - rva; }
    bool OwnCode(const void* address, std::size_t bytes = 1) const {
        const auto ptr = reinterpret_cast<std::uintptr_t>(address), start = reinterpret_cast<std::uintptr_t>(base);
        if (ptr < start || !Contains(ptr - start, bytes)) return false;
        for (const auto& s : sections)
            if ((s.Characteristics & IMAGE_SCN_MEM_EXECUTE) && ptr - start >= s.VirtualAddress &&
                ptr - start - s.VirtualAddress <= s.Misc.VirtualSize && bytes <= s.Misc.VirtualSize - (ptr - start - s.VirtualAddress)) return true;
        return false;
    }
    bool Open(HMODULE module) {
        base = reinterpret_cast<std::uint8_t*>(module); sections.clear(); size = 0;
        IMAGE_DOS_HEADER dos{};
        if (!base || !Read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 1024*1024 ||
            !Read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        size = nt.OptionalHeader.SizeOfImage;
        if (!size || size > 512*1024*1024 || reinterpret_cast<std::uintptr_t>(base) > UINTPTR_MAX - size) return false;
        const auto table = static_cast<std::size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        if (!nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96 || !Contains(table, nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER))) return false;
        for (std::size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER section{};
            if (!Read(base + table + i*sizeof(section), section) || !Contains(section.VirtualAddress, section.Misc.VirtualSize)) return false;
            sections.push_back(section);
        }
        return true;
    }
    // Discover a unique import by name; never store a version-specific IAT RVA.
    void** Import(const char* name) const {
        const auto dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !Contains(dir.VirtualAddress, dir.Size)) return nullptr;
        void** found{};
        for (std::size_t p = 0; p + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= dir.Size; p += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            IMAGE_IMPORT_DESCRIPTOR d{};
            if (!Read(base + dir.VirtualAddress + p, d)) return nullptr;
            if (!d.Name) break;
            if (!d.OriginalFirstThunk || !d.FirstThunk) return nullptr;
            for (std::size_t i = 0; i < size / sizeof(IMAGE_THUNK_DATA64); ++i) {
                const auto offset = i * sizeof(IMAGE_THUNK_DATA64);
                if (!Contains(static_cast<std::size_t>(d.OriginalFirstThunk) + offset, 8) || !Contains(static_cast<std::size_t>(d.FirstThunk) + offset, 8)) return nullptr;
                IMAGE_THUNK_DATA64 thunk{};
                if (!Read(base + d.OriginalFirstThunk + offset, thunk)) return nullptr;
                if (!thunk.u1.AddressOfData) break;
                if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal)) continue;
                const auto bytes = std::strlen(name) + 1;
                if (thunk.u1.AddressOfData > size || !Contains(static_cast<std::size_t>(thunk.u1.AddressOfData), sizeof(WORD) + bytes)) return nullptr;
                if (Equal(base + thunk.u1.AddressOfData + sizeof(WORD), {reinterpret_cast<const std::uint8_t*>(name), bytes})) {
                    if (found) return nullptr;
                    found = reinterpret_cast<void**>(base + d.FirstThunk + offset);
                }
            }
        }
        return found;
    }
};

// Startup-only transaction. Storage is reserved before publication. Destruction
// never attempts unsafe DLL-detach restoration; its owner stays process-resident.
class Transaction {
    struct Write {
        std::uint8_t* address;
        std::vector<std::uint8_t> before, after;
        DWORD protection{};
        bool touched{};
    };
    std::vector<Write> writes_;
    bool attempted_{}, unsafe_{};
    static bool Region(const Write& w, DWORD& protection) noexcept {
        MEMORY_BASIC_INFORMATION region{};
        const auto start = reinterpret_cast<std::uintptr_t>(w.address);
        if (!VirtualQuery(w.address, &region, sizeof(region)) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || start < reinterpret_cast<std::uintptr_t>(region.BaseAddress)) return false;
        const auto offset = start - reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (offset > region.RegionSize || w.before.size() > region.RegionSize - offset) return false;
        protection = region.Protect; return true;
    }
    static bool RestoreProtection(Write& w) noexcept {
        DWORD ignored{}, observed{};
        const auto effective = [](DWORD protection) {
            // A private copy of an image's WRITECOPY page is reported as
            // READWRITE. These grant the same access; preserve execute/modifiers.
            if (protection & PAGE_WRITECOPY) return (protection & ~PAGE_WRITECOPY) | PAGE_READWRITE;
            if (protection & PAGE_EXECUTE_WRITECOPY) return (protection & ~PAGE_EXECUTE_WRITECOPY) | PAGE_EXECUTE_READWRITE;
            return protection;
        };
        for (int n = 0; n < 2; ++n)
            if (VirtualProtect(w.address, w.before.size(), w.protection, &ignored) && Region(w, observed) && effective(observed) == effective(w.protection)) return true;
        return false;
    }
    static bool Store(Write& w, bool undo) noexcept {
        const auto& before = undo ? w.after : w.before;
        const auto& after = undo ? w.before : w.after;
        if (!Equal(w.address, before)) return false;
        DWORD ignored{};
        const bool code = (w.protection & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (!VirtualProtect(w.address, before.size(), code ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &ignored)) return false;
        w.touched = true;
        bool ok = Copy(w.address, before.data(), before.size()); // private copy-on-write page before interlocked access
        if (ok && before.size() == sizeof(void*) && (reinterpret_cast<std::uintptr_t>(w.address) % alignof(void*) == 0)) {
            void* expected{}, *replacement{};
            std::memcpy(&expected, before.data(), sizeof(expected)); std::memcpy(&replacement, after.data(), sizeof(replacement));
            __try { ok = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(w.address), replacement, expected) == expected; }
            __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        } else if (ok) ok = Copy(w.address, after.data(), after.size());
        if (code && !FlushInstructionCache(GetCurrentProcess(), w.address, after.size())) ok = false;
        const bool restored = RestoreProtection(w);
        return ok && restored && Equal(w.address, after);
    }
public:
    void Add(void* address, std::span<const std::uint8_t> before, std::span<const std::uint8_t> after) {
        if (attempted_ || !address || before.empty() || before.size() != after.size()) throw std::invalid_argument("invalid startup patch");
        writes_.push_back({static_cast<std::uint8_t*>(address), {before.begin(), before.end()}, {after.begin(), after.end()}});
    }
    bool Commit() noexcept {
        if (attempted_ || writes_.empty()) return false;
        attempted_ = true;
        for (auto& w : writes_)
            if (!Region(w, w.protection) || !Equal(w.address, w.before)) return false;
        for (auto& w : writes_) {
            if (Store(w, false)) continue;
            Rollback(); return false;
        }
        return true;
    }
    bool Rollback() noexcept {
        bool ok = true;
        for (auto i = writes_.rbegin(); i != writes_.rend(); ++i) {
            if (!i->touched) continue;
            if (Equal(i->address, i->before)) { if (!RestoreProtection(*i)) ok = false; }
            else if (!Store(*i, true)) ok = false;
        }
        unsafe_ |= !ok; return ok;
    }
    bool Unsafe() const { return unsafe_; }
    std::size_t Count() const { return writes_.size(); }
};
} // namespace trp::ampere::memory
