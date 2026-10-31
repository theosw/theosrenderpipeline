#include "NeuralRuntimeKernels.h"
#include "NeuralAdmission.h"
#include "NeuralRuntimeKernelsInternal.h"
#include "NeuralRuntimeKernelRegistry.h"
#include "NeuralFaultInjection.h"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <span>

namespace TheosRenderPipeline::NeuralRendering
{
    namespace
    {
        using namespace KernelPacks;
        // Pinning the image also prevents HMODULE address reuse. The raw storage
        // intentionally has process lifetime, including failed feature retirement.
        std::mutex installationMutex;
        InstallationRegistry installations;

        bool Copy(void* destination, const void* source, std::size_t size)
        {
            __try { std::memcpy(destination, source, size); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        struct WinMemory final : Memory
        {
            std::uint8_t* base;
            const char* failedOperation{"none"};
            std::uint32_t failedRva{};
            DWORD error{}, observed{}, expected{};
            explicit WinMemory(HMODULE module) : base(reinterpret_cast<std::uint8_t*>(module)) {}
            bool Read(std::uint32_t rva, std::span<std::uint8_t> bytes) override
            {
                if (rva > ImageSize || bytes.size() > ImageSize-rva) { return false; }
                auto address = reinterpret_cast<std::uintptr_t>(base)+rva;
                const auto end = address+bytes.size();
                while (address < end) {
                    MEMORY_BASIC_INFORMATION info{};
                    // Reading a guard page consumes its guard bit even if SEH
                    // catches the exception. Refuse before touching it.
                    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
                        info.AllocationBase != base || info.State != MEM_COMMIT || info.Type != MEM_IMAGE ||
                        (info.Protect & PAGE_GUARD) || !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) { return false; }
                    const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
                    if (regionEnd <= address) { return false; }
                    address = (std::min)(end, regionEnd);
                }
                return Copy(bytes.data(), base+rva, bytes.size());
            }
            bool Write(std::uint32_t rva, std::span<const std::uint8_t> bytes) override
            {
                return rva <= ImageSize && bytes.size() <= ImageSize-rva && Copy(base+rva, bytes.data(), bytes.size());
            }
            bool Protect(std::uint32_t page, DWORD protection, DWORD& old) override
            {
                MEMORY_BASIC_INFORMATION info{};
                const bool ok = page < ImageSize && VirtualQuery(base+page, &info, sizeof(info)) &&
                    info.AllocationBase == base && info.State == MEM_COMMIT && info.Type == MEM_IMAGE &&
                    !(info.Protect & PAGE_GUARD) && VirtualProtect(base+page, PageSize, protection, &old);
                if (!ok) { failedOperation = "VirtualProtect"; failedRva = page; error = GetLastError(); observed = info.Protect; expected = protection; }
                return ok;
            }
            bool ProtectionIs(std::uint32_t page, DWORD protection) override
            {
                MEMORY_BASIC_INFORMATION info{};
                const bool ok = VirtualQuery(base+page, &info, sizeof(info)) && ProtectionEquivalent(info.Protect, protection);
                if (!ok) { failedOperation = "protection readback"; failedRva = page; error = GetLastError(); observed = info.Protect; expected = protection; }
                return ok;
            }
            bool Flush() override
            {
                if (FaultInjection::Hit(FaultInjection::Point::KernelPublication)) { return false; }
                return FlushInstructionCache(GetCurrentProcess(), base, ImageSize) != 0;
            }
        };

        std::string Digest(std::span<const std::uint8_t> bytes)
        {
            BCRYPT_ALG_HANDLE algorithm{};
            BCRYPT_HASH_HANDLE hash{};
            if (bytes.size() > ULONG_MAX || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) { return {}; }
            std::array<std::uint8_t, 32> digest{};
            bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
            if (ok) { ok = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0; }
            if (ok) { ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0; }
            if (hash) { BCryptDestroyHash(hash); }
            BCryptCloseAlgorithmProvider(algorithm, 0);
            constexpr char hex[] = "0123456789ABCDEF";
            if (!ok) { return {}; }
            std::string result(64, '0');
            for (std::size_t i = 0; i < digest.size(); ++i) {
                result[2*i] = hex[digest[i] >> 4]; result[2*i+1] = hex[digest[i] & 15];
            }
            return result;
        }
        bool HashIs(std::span<const std::uint8_t> bytes, std::string_view expected) { return expected.size() == 64 && Digest(bytes) == expected; }

        struct File
        {
            HANDLE handle{INVALID_HANDLE_VALUE}, mapping{};
            const std::uint8_t* data{};
            std::size_t size{};
            explicit File(const std::filesystem::path& path, std::size_t expectedSize)
            {
                // Hold the same file, denying replacement/writes through the
                // complete hash, image check and publication operation.
                handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                LARGE_INTEGER length{};
                if (handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(handle, &length) || length.QuadPart != static_cast<LONGLONG>(expectedSize)) { return; }
                mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
                if (mapping) { data = static_cast<const std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)); }
                if (data) { size = expectedSize; }
            }
            ~File()
            {
                if (data) { UnmapViewOfFile(data); }
                if (mapping) { CloseHandle(mapping); }
                if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); }
            }
            File(const File&) = delete;
            File& operator=(const File&) = delete;
            std::span<const std::uint8_t> Bytes() const { return {data, size}; }
        };

        bool SameBytes(WinMemory& memory, std::uint32_t rva, std::span<const std::uint8_t> expected)
        {
            std::array<std::uint8_t, 65536> buffer{};
            while (!expected.empty()) {
                const auto count = (std::min)(buffer.size(), expected.size());
                if (!memory.Read(rva, std::span(buffer).first(count)) || !std::equal(buffer.begin(), buffer.begin()+count, expected.begin())) { return false; }
                expected = expected.subspan(count);
                rva += static_cast<std::uint32_t>(count);
            }
            return true;
        }

        bool OriginalImage(WinMemory& memory, std::span<const std::uint8_t> file)
        {
            // This parser is reached only after the complete file's pinned hash.
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || static_cast<std::size_t>(dos->e_lfanew) > file.size()-sizeof(IMAGE_NT_HEADERS64)) { return false; }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data()+dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
                nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->OptionalHeader.SizeOfImage != ImageSize ||
                nt->OptionalHeader.SizeOfHeaders > file.size()) { return false; }
            // Windows updates OptionalHeader.ImageBase in a relocated image.
            // Normalize only that loader-owned field; all other header bytes
            // must still match the identified original file.
            std::vector<std::uint8_t> headers(file.begin(), file.begin()+nt->OptionalHeader.SizeOfHeaders);
            const auto baseOffset = static_cast<std::size_t>(dos->e_lfanew)+offsetof(IMAGE_NT_HEADERS64, OptionalHeader)+offsetof(IMAGE_OPTIONAL_HEADER64, ImageBase);
            if (baseOffset > headers.size() || sizeof(std::uint64_t) > headers.size()-baseOffset) { return false; }
            const auto loadedBase = reinterpret_cast<std::uint64_t>(memory.base);
            std::memcpy(headers.data()+baseOffset, &loadedBase, sizeof(loadedBase));
            if (!SameBytes(memory, 0, headers)) { return false; }
            const auto* sections = IMAGE_FIRST_SECTION(nt);
            const auto table = static_cast<std::size_t>(reinterpret_cast<const std::uint8_t*>(sections)-file.data());
            if (table > file.size() || nt->FileHeader.NumberOfSections > (file.size()-table)/sizeof(IMAGE_SECTION_HEADER)) { return false; }
            for (const auto& b : Bundles) {
                bool found = false;
                for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                    const auto& s = sections[i];
                    if (b.original < s.VirtualAddress) { continue; }
                    const auto offset = b.original-s.VirtualAddress;
                    if (offset > s.SizeOfRawData || b.bytes > s.SizeOfRawData-offset) { continue; }
                    const auto raw = static_cast<std::uint64_t>(s.PointerToRawData)+offset;
                    if (raw > file.size() || b.bytes > file.size()-raw || !SameBytes(memory, b.original, file.subspan(static_cast<std::size_t>(raw), b.bytes))) { return false; }
                    found = true;
                    break;
                }
                if (!found) { return false; }
            }
            return true;
        }

        struct FreeStorage { void operator()(void* address) const { if (address) { VirtualFree(address, 0, MEM_RELEASE); } } };
        using Storage = std::unique_ptr<void, FreeStorage>;
        Storage NearbyStorage(std::uintptr_t module, std::size_t PackSize)
        {
            SYSTEM_INFO system{};
            GetSystemInfo(&system);
            if (system.dwPageSize != PageSize) { return {}; }
            const auto granularity = static_cast<std::uintptr_t>(system.dwAllocationGranularity);
            const auto minimum = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
            const auto maximum = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
            // Leave margin for instruction RVAs and bundle offsets at both
            // signed-displacement limits, including a below-image allocation.
            constexpr std::uintptr_t reach = 0x7F000000;
            const auto low = (std::max)(minimum, module > reach ? module-reach : minimum);
            const auto high = (std::min)(maximum-PackSize, module+reach-PackSize);
            const auto search = [&](std::uintptr_t start, std::uintptr_t limit) -> Storage {
                auto address = (start+granularity-1) & ~(granularity-1);
                while (address <= limit) {
                    MEMORY_BASIC_INFORMATION info{};
                    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) { break; }
                    const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
                    if (regionEnd <= address) { break; }
                    if (info.State == MEM_FREE && regionEnd-address >= PackSize) {
                        if (auto* storage = VirtualAlloc(reinterpret_cast<void*>(address), PackSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) { return Storage(storage); }
                    }
                    address = (regionEnd+granularity-1) & ~(granularity-1);
                }
                return {};
            };
            if (auto above = search(module, high)) { return above; }
            return search(low, (std::min)(high, module));
        }
    }

    bool RuntimeKernelsSupported() {
#if defined(TRP_STANDARD_RESEARCH)
        return false;
#else
        return true;
#endif
    }

    LaunchRequirements RuntimeLaunchRequirements(HMODULE module)
    {
        std::scoped_lock lock(installationMutex);
        const auto* installed = installations.Find(module);
        return installed && installed->result.applied ? installed->launches : LaunchRequirements{};
    }

    int RuntimeKernelBundle(HMODULE module, const void* data, std::uint32_t bytes)
    {
        std::scoped_lock lock(installationMutex);
        const auto* installed = installations.Find(module);
        if (!installed || !installed->result.applied) { return -1; }
        const auto* pack = static_cast<const std::uint8_t*>(installed->storage);
        for (int i=0; i<15; ++i) {
            std::uint64_t offset{}, size{};
            std::memcpy(&offset, pack+48+i*24+8, 8);
            std::memcpy(&size, pack+48+i*24+16, 8);
            if (data == pack+offset && size == bytes) { return i; }
        }
        return -1;
    }

    RuntimeKernelResult ApplyRuntimeKernels(HMODULE module, const std::filesystem::path& packPath, bool newlyLoaded, bool research)
    {
        using namespace KernelPacks;
#if defined(TRP_STANDARD_RESEARCH)
        if (!research) { return {false, false, "NR 310.9.16 on RTX 20/30/40 needs the Universal edition"}; }
#endif
        const bool pascalPack = !research && packPath.filename() == kPascalPackName;
        std::scoped_lock lock(installationMutex);
        WinMemory memory(module);
        return installations.Apply(module, newlyLoaded, [&](Installation& installed) -> std::optional<RuntimeKernelResult> {
            if (!research && installed.digest != (pascalPack ? kPascalPackSha256 : kKernelPackSha256)) {
                return RuntimeKernelResult{false, false, "a different NR compatibility pack is already installed; restart required"};
            }
            if (installed.result.applied) {
                MEMORY_BASIC_INFORMATION info{};
                if (!Matches(memory, installed.plan, true) || !VirtualQuery(installed.storage, &info, sizeof(info)) ||
                    info.State != MEM_COMMIT || info.Protect != PAGE_READONLY || info.RegionSize < installed.storageBytes ||
                    !HashIs({static_cast<const std::uint8_t*>(installed.storage), installed.storageBytes}, installed.digest)) {
                    installed.result = {false, true, "installed NR runtime or storage changed; relaunch required"};
                }
            }
            return std::nullopt;
        }, [&](Installation*) -> RuntimeKernelResult {
        std::wstring filename(32768, L'\0');
        const auto length = GetModuleFileNameW(module, filename.data(), static_cast<DWORD>(filename.size()));
        if (!length || length >= filename.size()) { return {false, false, "cannot identify the loaded NR module"}; }
        filename.resize(length);
        File runtime(filename, 266424944);
        std::error_code sizeError;
        const auto packSize = research ? static_cast<std::size_t>(std::filesystem::file_size(packPath, sizeError)) : (pascalPack ? kPascalPackBytes : PackSize);
        if (sizeError || packSize < 512 || packSize > ULONG_MAX) { return {false, false, "NR compatibility kernel pack is missing or outside supported size bounds"}; }
        File pack(packPath, packSize);
        if (!runtime.data || !HashIs(runtime.Bytes(), kKernelRuntimeSha256)) { return {false, false, "loaded NR file is not the pinned NVIDIA 310.9.16 runtime"}; }
        // Research packs are unpinned; production packs must match their pinned hash.
        if (!pack.data || (!research && !HashIs(pack.Bytes(), pascalPack ? kPascalPackSha256 : kKernelPackSha256))) { return {false, false, "NR compatibility kernel pack is missing or its SHA-256 differs"}; }
        if (!OriginalImage(memory, runtime.Bytes())) { return {false, false, "loaded NR image headers or original kernels differ"}; }
        auto storage = NearbyStorage(reinterpret_cast<std::uintptr_t>(module), packSize);
        if (!storage) { return {false, false, "no memory available within NR bundle instruction reach"}; }
        std::memcpy(storage.get(), pack.data, packSize);
        DWORD old{};
        if (!VirtualProtect(storage.get(), packSize, PAGE_READONLY, &old)) { return {false, false, "NR compatibility kernel storage could not be made read-only"}; }
        Installation pending;
        pending.storageBytes = packSize;
        pending.research = research;
        pending.digest = Digest(pack.Bytes());
        if (pending.digest.size() != 64) { return {false, false, "NR kernel pack hashing failed; NR off"}; }
        if (!MakePlan(pack.Bytes(), reinterpret_cast<std::uintptr_t>(module), reinterpret_cast<std::uintptr_t>(storage.get()),
                pending.plan, pending.result.status, packSize, research || pascalPack)) { return pending.result; }
        std::uint32_t version{};
        std::memcpy(&version, pack.data+8, 4);
        if (version == 2) {
            std::uint64_t offset{}, bytes{};
            std::memcpy(&offset, pack.data+48+14*24+8, 8);
            std::memcpy(&bytes, pack.data+48+14*24+16, 8);
            if (!ReadLaunchRequirements(pack.Bytes().subspan(static_cast<std::size_t>(offset+bytes)), pending.launches, pending.result.status)) { return pending.result; }
        }
        if (!Matches(memory, pending.plan, false)) { return {false, false, "original NR patch bytes differ"}; }
        // Allocate registry/status before publication. Pin before the first write
        // so both successful and uncertain transactions can retain all targets.
        pending.result.status.reserve(128);
        auto* entry = installations.Retain(module, std::move(pending), [&] {
            HMODULE pinned{};
            return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) && pinned == module;
        });
        if (!entry) { return {false, false, "NR runtime could not be retained for the process lifetime"}; }
        auto& installed = *entry;
        const auto result = Publish(memory, installed.plan, installed.result.status);
        installed.result.applied = result == Publication::Applied;
        installed.result.unsafe = result == Publication::Unsafe;
        if (result != Publication::Refused) { installed.storage = storage.release(); }
        if (result != Publication::Applied) {
            installed.result.status += std::format("; {} RVA=0x{:X} error={} protection=0x{:X} expected=0x{:X}",
                memory.failedOperation, memory.failedRva, memory.error, memory.observed, memory.expected);
        }
        return installed.result;
        });
    }
}
