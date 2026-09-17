#include "midpoint_fix.h"
#include "dlssg_provider_policy.h"

#include <bcrypt.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace midpoint_fix
{
namespace
{
constexpr size_t kKernelCount = 25;
constexpr size_t kTemporalSlot = 9;
constexpr size_t kDescriptorBytes = 48;
constexpr size_t kNameBytes = 96;
constexpr uint64_t kMaximumFatbinBytes = 64ull * 1024ull * 1024ull;
constexpr size_t kOutputCapacity = 256 * 1024;
constexpr size_t kScratchCapacity = 256 * 1024;
constexpr char kJoinLabel[] = "$L__BB0_3:";
constexpr char kMidpointBits[] = "0f3F000000";
constexpr char kLegacyTemporalInput[] =
    "ld.param.f32 %f134, [main_kernel_param_0+32];\r\n"
    "mov.f32 %f135, 0f3F800000;\r\n"
    "sub.ftz.f32 %f136, %f135, %f134;\r\n";
constexpr char kNamedTemporalInput[] =
    "ld.param.f32 %f134, [Kernel_EstimateIntermMvecsScatter_param_0+32];\r\n"
    "mov.f32 %f135, 0f3F800000;\r\n"
    "sub.ftz.f32 %f136, %f135, %f134;\r\n";
constexpr size_t kTemporalMultiplyCount = 104;
constexpr size_t kTemporalDirectionCount = kTemporalMultiplyCount / 2;

struct TemporalProviderProfile
{
    size_t temporalSlot = 0;
    bool namedKernelDescriptors = false;
    uint64_t sourceFatbinBytes = 0;
    const char* sourceFatbinSha256 = nullptr;
    const char* sourcePtxSha256 = nullptr;
    const char* outputFatbinSha256 = nullptr;
    const char* temporalInput = nullptr;
    const char* curr2PrevScale = nullptr;
    const char* prev2CurrScale = nullptr;
    size_t sm120EntryBytes = 0;
    uint64_t sm120PayloadBytes = 0;
    uint32_t sm120CompressedBytes = 0;
    uint64_t sm120RawBytes = 0;
    uint64_t sm89PayloadBytes = 0;
    uint32_t sm89CompressedBytes = 0;
    uint64_t sm89RawBytes = 0;
};

// Match the program being transformed; DLL versions do not select a profile.
constexpr std::array<TemporalProviderProfile, 2> kTemporalProfiles{{
    {
        kTemporalSlot, false, 98408,
        "5A8E0284AAB8AC14FC82B0504BBEEF25D2FCE1D13A1C11D8BDB3F91FEE8145FC",
        "46C05996A2EF199BBE39378681734ED5EE757655DE70410D9900D85BA91222F1",
        "19FB3CD5500BFD1FC88F96C36B381E096D6AB1DFC7B1DB02A04D40A00849104B",
        kLegacyTemporalInput, "%f136", "%f134",
        28128, 28024, 28017, 90490, 30384, 30379, 99362,
    },
    {
        kTemporalSlot, true, 98704,
        "FBA7599CC9CDC1EED947AD5ADB94436052E8C17AD7FD268C2590AB36A0C731E9",
        "D1EB7A60C57915238AA228511DA16386F95AAAA5A0D1C1B7903F1DA8FB9519B4",
        "579ACF3317ED39B45AC0ABBC70F4DC812A53AE0F8B11E528B7B5542681E8BEF4",
        kNamedTemporalInput, "%f136", "%f134",
        28144, 28040, 28040, 90732, 30408, 30402, 99626,
    },
}};

enum class Failure : uint32_t
{
    eNone = 0,
    eAdapterUnavailable = 1,
    eAdapterNotAda = 2,
    eProviderIdentity = 3,
    eProviderLayout = 4,
    eSourceIdentity = 5,
    eDecompression = 6,
    eTemporalLayout = 7,
    eOutputIdentity = 8,
    eAllocation = 9,
    ePublication = 10,
    eRestartRequired = 11,
    eProviderNotReady = 12,
};

struct PublishResult
{
    bool success = false;
    bool replacementWasVisible = false;
};

std::atomic<LogCallback> gLogCallback{nullptr};
std::atomic<bool> gAdapterVerified{false};
std::atomic<bool> gReady{false};
std::atomic<uint32_t> gFailure{
    static_cast<uint32_t>(Failure::eAdapterUnavailable)};
std::atomic<uint64_t> gAdapterLuid{0};
std::mutex gMutex;
HMODULE gProvider = nullptr;
HMODULE gPinnedProvider = nullptr;
uint64_t gPublishedAdapterLuid = 0;
uintptr_t gDescriptorEntry = 0;
uintptr_t gReplacementDescriptor = 0;
void* gAllocation = nullptr;

void Log(const wchar_t* format, ...) noexcept
{
    const LogCallback callback = gLogCallback.load(std::memory_order_acquire);
    if (!callback)
        return;
    wchar_t message[1024]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);
    callback(message);
}

bool SafeCopy(void* destination, const void* source, size_t bytes) noexcept
{
    if (!destination || !source || bytes == 0)
        return false;
    __try
    {
        std::memcpy(destination, source, bytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T>
bool SafeRead(uintptr_t address, T& value) noexcept
{
    return SafeCopy(&value, reinterpret_cast<const void*>(address),
        sizeof(value));
}

uint16_t ReadU16(const uint8_t* bytes) noexcept
{
    uint16_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

uint32_t ReadU32(const uint8_t* bytes) noexcept
{
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

uint64_t ReadU64(const uint8_t* bytes) noexcept
{
    uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool Sha256Equals(const uint8_t* bytes, size_t count,
    const char* expected) noexcept
{
    if (!bytes || count == 0 || count > ULONG_MAX || !expected
        || std::strlen(expected) != 64)
        return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    std::array<uint8_t, 32> digest{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm,
        BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0)
    {
        status = BCryptHash(algorithm, nullptr, 0,
            const_cast<PUCHAR>(bytes), static_cast<ULONG>(count),
            digest.data(), static_cast<ULONG>(digest.size()));
    }
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
        return false;

    static constexpr char kDigits[] = "0123456789ABCDEF";
    for (size_t index = 0; index < digest.size(); ++index)
    {
        if (expected[index * 2] != kDigits[digest[index] >> 4]
            || expected[index * 2 + 1] != kDigits[digest[index] & 0x0F])
            return false;
    }
    return true;
}

bool ImageSize(HMODULE module, uint32_t& imageSize) noexcept
{
    imageSize = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    if (!base || !SafeRead(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0
        || static_cast<uintptr_t>(dos.e_lfanew) > UINTPTR_MAX - base)
        return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!SafeRead(base + static_cast<uintptr_t>(dos.e_lfanew), nt)
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt.OptionalHeader.SizeOfImage < sizeof(nt))
        return false;
    imageSize = nt.OptionalHeader.SizeOfImage;
    return true;
}

bool IsReadOnlyImageAddress(HMODULE module, uintptr_t address) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!module || !address
        || VirtualQuery(reinterpret_cast<const void*>(address), &memory,
            sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE
        || memory.AllocationBase != module)
        return false;
    const DWORD protection = memory.Protect & 0xFFu;
    return protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
}

bool KernelIdentityMatches(const TemporalProviderProfile& profile, size_t slot,
    const char* descriptorName, const char* entryName) noexcept
{
    if (!descriptorName || !entryName || slot >= kKernelCount)
        return false;
    // Other entries establish table structure, not temporal program identity.
    if (slot != profile.temporalSlot)
        return descriptorName[0] != '\0' && entryName[0] != '\0';
    if (!profile.namedKernelDescriptors)
    {
        return std::strcmp(descriptorName, "dlfg_kernel") == 0
            && std::strcmp(entryName, "main_kernel") == 0;
    }

    return std::strcmp(descriptorName, "EstimateIntermMvecsScatter") == 0
        && std::strcmp(entryName, "Kernel_EstimateIntermMvecsScatter") == 0;
}

bool DescriptorMatches(uintptr_t moduleBase, uintptr_t moduleEnd,
    uintptr_t descriptor, size_t slot,
    const TemporalProviderProfile& profile) noexcept
{
    if (!moduleBase || moduleEnd <= moduleBase
        || descriptor < moduleBase || moduleEnd - moduleBase < kDescriptorBytes
        || descriptor > moduleEnd - kDescriptorBytes)
        return false;

    std::array<uint8_t, kDescriptorBytes> bytes{};
    if (!SafeCopy(bytes.data(), reinterpret_cast<const void*>(descriptor),
            bytes.size()))
        return false;
    uintptr_t name = 0;
    uintptr_t fatbin = 0;
    uintptr_t entry = 0;
    uint32_t suppliedSize = 0;
    std::memcpy(&name, bytes.data(), sizeof(name));
    std::memcpy(&fatbin, bytes.data() + 0x08, sizeof(fatbin));
    std::memcpy(&suppliedSize, bytes.data() + 0x10, sizeof(suppliedSize));
    std::memcpy(&entry, bytes.data() + 0x18, sizeof(entry));

    char descriptorName[kNameBytes]{};
    char entryName[kNameBytes]{};
    if (!name || !entry || !fatbin || moduleEnd - moduleBase < kNameBytes
        || name < moduleBase || name > moduleEnd - kNameBytes
        || entry < moduleBase || entry > moduleEnd - kNameBytes
        || fatbin < moduleBase || fatbin > moduleEnd - 16
        || !SafeCopy(descriptorName, reinterpret_cast<const void*>(name),
            sizeof(descriptorName) - 1)
        || !SafeCopy(entryName, reinterpret_cast<const void*>(entry),
            sizeof(entryName) - 1)
        || !KernelIdentityMatches(profile, slot, descriptorName, entryName))
        return false;

    std::array<uint8_t, 16> header{};
    if (!SafeCopy(header.data(), reinterpret_cast<const void*>(fatbin),
            header.size())
        || ReadU32(header.data()) != 0xBA55ED50u)
        return false;
    const uint16_t headerSize = ReadU16(header.data() + 6);
    const uint64_t payloadSize = ReadU64(header.data() + 8);
    const uint64_t totalSize = static_cast<uint64_t>(headerSize) + payloadSize;
    if (headerSize < header.size() || totalSize < headerSize
        || totalSize > kMaximumFatbinBytes
        || totalSize > static_cast<uint64_t>(moduleEnd - fatbin)
        || (suppliedSize != 0 && suppliedSize != totalSize))
        return false;
    return slot != profile.temporalSlot
        || (totalSize == profile.sourceFatbinBytes
            && Sha256Equals(reinterpret_cast<const uint8_t*>(fatbin),
                static_cast<size_t>(totalSize), profile.sourceFatbinSha256));
}

bool DescriptorTableMatches(uintptr_t moduleBase, uintptr_t moduleEnd,
    uintptr_t table, const TemporalProviderProfile& profile) noexcept
{
    constexpr size_t kTableBytes = kKernelCount * sizeof(uintptr_t);
    if (!table || moduleEnd <= moduleBase || table < moduleBase
        || moduleEnd - moduleBase < kTableBytes || table > moduleEnd - kTableBytes)
        return false;
    for (size_t slot = 0; slot < kKernelCount; ++slot)
    {
        uintptr_t descriptor = 0;
        if (!SafeRead(table + slot * sizeof(uintptr_t), descriptor)
            || !DescriptorMatches(moduleBase, moduleEnd, descriptor, slot,
                profile))
            return false;
    }
    return true;
}

struct TemporalSource
{
    uintptr_t entry = 0;
    const TemporalProviderProfile* profile = nullptr;
};

TemporalSource FindDescriptorEntry(HMODULE module, uint32_t imageSize) noexcept
{
    constexpr size_t kTableBytes = kKernelCount * sizeof(uintptr_t);
    if (!module || imageSize < kTableBytes)
        return {};
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    if (base > UINTPTR_MAX - imageSize)
        return {};
    const uintptr_t end = base + imageSize;

    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!SafeRead(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0
        || static_cast<uintptr_t>(dos.e_lfanew) > UINTPTR_MAX - base
        || !SafeRead(base + static_cast<uintptr_t>(dos.e_lfanew), nt)
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return {};
    const size_t sectionOffset = static_cast<size_t>(dos.e_lfanew)
        + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
        + nt.FileHeader.SizeOfOptionalHeader;
    if (sectionOffset > imageSize
        || nt.FileHeader.NumberOfSections
            > (imageSize - sectionOffset) / sizeof(IMAGE_SECTION_HEADER))
        return {};

    TemporalSource match{};
    for (uint16_t index = 0; index < nt.FileHeader.NumberOfSections; ++index)
    {
        IMAGE_SECTION_HEADER section{};
        if (!SafeRead(base + sectionOffset
                + index * sizeof(IMAGE_SECTION_HEADER), section))
            return {};
        if ((section.Characteristics & IMAGE_SCN_MEM_READ) == 0
            || (section.Characteristics
                & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) != 0
            || section.VirtualAddress >= imageSize)
            continue;
        const size_t requested = std::max<size_t>(
            section.Misc.VirtualSize, section.SizeOfRawData);
        const size_t sectionSize = std::min<size_t>(requested,
            imageSize - section.VirtualAddress);
        if (sectionSize < kTableBytes)
            continue;
        const uintptr_t begin = base + section.VirtualAddress;
        for (size_t offset = 0; offset + kTableBytes <= sectionSize;
             offset += alignof(uintptr_t))
        {
            const uintptr_t table = begin + offset;
            for (const auto& profile : kTemporalProfiles)
            {
                uintptr_t descriptor = 0;
                const uintptr_t entry = table + profile.temporalSlot * sizeof(uintptr_t);
                if (!SafeRead(entry, descriptor)
                    || !DescriptorMatches(base, end, descriptor, profile.temporalSlot, profile)
                    || !DescriptorTableMatches(base, end, table, profile))
                    continue;
                if (match.entry)
                    return {};
                match = {entry, &profile};
            }
        }
    }
    return match;
}

bool DecompressNvidiaLz(const uint8_t* input, size_t inputSize,
    uint8_t* output, size_t outputSize) noexcept
{
    if (!input || !output || inputSize == 0 || outputSize == 0)
        return false;
    size_t inputOffset = 0;
    size_t outputOffset = 0;
    while (inputOffset < inputSize)
    {
        const uint8_t token = input[inputOffset++];
        size_t literalBytes = token >> 4;
        if (literalBytes == 15)
        {
            uint8_t extension = 0;
            do
            {
                if (inputOffset >= inputSize
                    || literalBytes > SIZE_MAX - input[inputOffset])
                    return false;
                extension = input[inputOffset++];
                literalBytes += extension;
            } while (extension == 0xFF);
        }
        if (literalBytes > inputSize - inputOffset
            || literalBytes > outputSize - outputOffset)
            return false;
        std::memcpy(output + outputOffset, input + inputOffset, literalBytes);
        inputOffset += literalBytes;
        outputOffset += literalBytes;
        if (inputOffset == inputSize)
            break;
        if (inputSize - inputOffset < 2)
            return false;
        const size_t backOffset = static_cast<size_t>(input[inputOffset])
            | (static_cast<size_t>(input[inputOffset + 1]) << 8);
        inputOffset += 2;
        if (backOffset == 0 || backOffset > outputOffset)
            return false;
        size_t matchBytes = 4 + (token & 0x0F);
        if ((token & 0x0F) == 15)
        {
            uint8_t extension = 0;
            do
            {
                if (inputOffset >= inputSize
                    || matchBytes > SIZE_MAX - input[inputOffset])
                    return false;
                extension = input[inputOffset++];
                matchBytes += extension;
            } while (extension == 0xFF);
        }
        if (matchBytes > outputSize - outputOffset)
            return false;
        for (size_t index = 0; index < matchBytes; ++index)
            output[outputOffset + index] =
                output[outputOffset + index - backOffset];
        outputOffset += matchBytes;
    }
    return inputOffset == inputSize && outputOffset == outputSize;
}

size_t FindUniqueBytes(const uint8_t* bytes, size_t count,
    const char* marker, size_t markerBytes) noexcept
{
    if (!bytes || !marker || markerBytes == 0 || markerBytes > count)
        return SIZE_MAX;
    size_t match = SIZE_MAX;
    for (size_t offset = 0; offset + markerBytes <= count; ++offset)
    {
        if (std::memcmp(bytes + offset, marker, markerBytes) != 0)
            continue;
        if (match != SIZE_MAX)
            return SIZE_MAX;
        match = offset;
    }
    return match;
}

bool BuildTemporalFatbin(uint8_t* fatbin, uint8_t* scratch,
    const TemporalProviderProfile& profile, uint32_t& outputBytes,
    Failure& failure) noexcept
{
    constexpr size_t kOuterHeaderBytes = 16;
    constexpr size_t kSm120EntryOffset = kOuterHeaderBytes;
    constexpr size_t kSm89HeaderBytes = 104;
    constexpr uint64_t kCompressedFlags = 0x2041;
    constexpr uint64_t kUncompressedFlags = 0x41;
    constexpr char kMulPrefix[] = "mul.ftz.f32 ";
    failure = Failure::eTemporalLayout;
    if (!fatbin || !scratch || ReadU32(fatbin) != 0xBA55ED50u
        || !profile.temporalInput || !profile.curr2PrevScale
        || !profile.prev2CurrScale
        || ReadU16(fatbin + 6) != kOuterHeaderBytes
        || ReadU64(fatbin + 8) + kOuterHeaderBytes
            != profile.sourceFatbinBytes)
        return false;
    const size_t kSm89EntryOffset =
        kSm120EntryOffset + profile.sm120EntryBytes;
    uint8_t* sm120 = fatbin + kSm120EntryOffset;
    uint8_t* sm89 = fatbin + kSm89EntryOffset;
    if (ReadU16(sm120) != 1 || ReadU32(sm120 + 4) != 104
        || ReadU64(sm120 + 8) != profile.sm120PayloadBytes
        || ReadU32(sm120 + 16) != profile.sm120CompressedBytes
        || ReadU32(sm120 + 28) != 120 || ReadU64(sm120 + 40) != 0x2041
        || ReadU64(sm120 + 56) != profile.sm120RawBytes
        || ReadU16(sm89) != 1 || ReadU32(sm89 + 4) != kSm89HeaderBytes
        || ReadU64(sm89 + 8) != profile.sm89PayloadBytes
        || ReadU32(sm89 + 16) != profile.sm89CompressedBytes
        || ReadU32(sm89 + 28) != 89
        || ReadU64(sm89 + 40) != kCompressedFlags
        || ReadU64(sm89 + 56) != profile.sm89RawBytes)
        return false;
    if (!DecompressNvidiaLz(sm89 + kSm89HeaderBytes,
            profile.sm89CompressedBytes, scratch, profile.sm89RawBytes))
    {
        failure = Failure::eDecompression;
        return false;
    }
    if (!Sha256Equals(scratch, profile.sm89RawBytes,
            profile.sourcePtxSha256))
    {
        failure = Failure::eSourceIdentity;
        return false;
    }

    const size_t label = FindUniqueBytes(scratch, profile.sm89RawBytes,
        kJoinLabel, sizeof(kJoinLabel) - 1);
    if (label == SIZE_MAX)
        return false;
    size_t insertion = label + sizeof(kJoinLabel) - 1;
    while (insertion < profile.sm89RawBytes && scratch[insertion] != '\n')
        ++insertion;
    if (insertion >= profile.sm89RawBytes)
        return false;
    ++insertion;

    std::array<size_t, kTemporalMultiplyCount> midpointOffsets{};
    size_t midpointCount = 0;
    for (size_t offset = 0;
         offset + sizeof(kMidpointBits) - 1 < profile.sm89RawBytes; ++offset)
    {
        if (std::memcmp(scratch + offset, kMidpointBits,
                sizeof(kMidpointBits) - 1) != 0
            || scratch[offset + sizeof(kMidpointBits) - 1] != ';')
            continue;
        size_t line = offset;
        while (line > 0 && scratch[line - 1] != '\n')
            --line;
        if (offset - line < sizeof(kMulPrefix) - 1
            || std::memcmp(scratch + line, kMulPrefix,
                sizeof(kMulPrefix) - 1) != 0
            || midpointCount >= midpointOffsets.size())
            continue;
        midpointOffsets[midpointCount++] = offset;
    }
    if (midpointCount != midpointOffsets.size()
        || midpointOffsets[0] <= insertion)
        return false;

    std::array<uint8_t, kSm89HeaderBytes> header{};
    std::memcpy(header.data(), sm89, header.size());
    uint8_t* destination = fatbin + kSm89EntryOffset + kSm89HeaderBytes;
    size_t sourceOffset = 0;
    size_t destinationOffset = 0;
    auto append = [&](const uint8_t* source, size_t bytes) noexcept {
        const size_t prefix = kSm89EntryOffset + kSm89HeaderBytes;
        if (!source || prefix > kOutputCapacity
            || destinationOffset > kOutputCapacity - prefix
            || bytes > kOutputCapacity - prefix - destinationOffset)
            return false;
        std::memcpy(destination + destinationOffset, source, bytes);
        destinationOffset += bytes;
        return true;
    };
    if (!append(scratch, insertion)
        || !append(reinterpret_cast<const uint8_t*>(profile.temporalInput),
            std::strlen(profile.temporalInput)))
        return false;
    sourceOffset = insertion;
    for (size_t index = 0; index < midpointOffsets.size(); ++index)
    {
        const size_t marker = midpointOffsets[index];
        if (marker < sourceOffset
            || !append(scratch + sourceOffset, marker - sourceOffset))
            return false;
        const char* scale = index < kTemporalDirectionCount
            ? profile.curr2PrevScale : profile.prev2CurrScale;
        if (!append(reinterpret_cast<const uint8_t*>(scale),
                std::strlen(scale)))
            return false;
        sourceOffset = marker + sizeof(kMidpointBits) - 1;
    }
    if (!append(scratch + sourceOffset,
            profile.sm89RawBytes - sourceOffset))
        return false;

    const size_t padded = (destinationOffset + 7) & ~size_t{7};
    const size_t finalSize = kSm89EntryOffset + kSm89HeaderBytes + padded;
    if (finalSize > kOutputCapacity || finalSize > UINT32_MAX)
        return false;
    std::memcpy(fatbin + kSm89EntryOffset, header.data(), header.size());
    std::memset(destination + destinationOffset, 0,
        padded - destinationOffset);
    const uint64_t padded64 = padded;
    const uint32_t zero32 = 0;
    const uint64_t zero64 = 0;
    std::memcpy(fatbin + kSm89EntryOffset + 8, &padded64, sizeof(padded64));
    std::memcpy(fatbin + kSm89EntryOffset + 16, &zero32, sizeof(zero32));
    std::memcpy(fatbin + kSm89EntryOffset + 40,
        &kUncompressedFlags, sizeof(kUncompressedFlags));
    std::memcpy(fatbin + kSm89EntryOffset + 56, &zero64, sizeof(zero64));
    const uint64_t payload = finalSize - kOuterHeaderBytes;
    std::memcpy(fatbin + 8, &payload, sizeof(payload));
    outputBytes = static_cast<uint32_t>(finalSize);
    failure = Failure::eNone;
    return true;
}

bool ProtectionMatches(uintptr_t address, DWORD expected) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!address
        || VirtualQuery(reinterpret_cast<const void*>(address), &memory,
            sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT)
        return false;
    constexpr DWORD kMask = 0xFFu | PAGE_GUARD | PAGE_NOCACHE
        | PAGE_WRITECOMBINE;
    return (memory.Protect & kMask) == (expected & kMask);
}

bool SetAndVerifyProtection(void* address, size_t bytes,
    DWORD protection) noexcept
{
    DWORD ignored = 0;
    VirtualProtect(address, bytes, protection, &ignored);
    return ProtectionMatches(reinterpret_cast<uintptr_t>(address), protection);
}

bool SetWritableProtection(void* address, size_t bytes) noexcept
{
    DWORD ignored = 0;
    VirtualProtect(address, bytes, PAGE_READWRITE, &ignored);
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory))
        return false;
    const DWORD protection = memory.Protect & 0xFFu;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY;
}

bool RestoreProtection(void* address, size_t bytes,
    DWORD protection) noexcept
{
    return SetAndVerifyProtection(address, bytes, protection)
        || SetAndVerifyProtection(address, bytes, protection);
}

PublishResult PublishPointer(uintptr_t address, uintptr_t expected,
    uintptr_t replacement) noexcept
{
    if (!address || !expected || !replacement
        || (address & (alignof(void*) - 1)) != 0)
        return {};
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory,
            sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT)
        return {};
    const DWORD oldProtection = memory.Protect;
    auto* destination = reinterpret_cast<void* volatile*>(address);
    void* page = reinterpret_cast<void*>(address);
    if (!SetWritableProtection(page, sizeof(void*)))
    {
        RestoreProtection(page, sizeof(void*), oldProtection);
        return {};
    }

    void* expectedPointer = reinterpret_cast<void*>(expected);
    void* replacementPointer = reinterpret_cast<void*>(replacement);
    uintptr_t current = 0;
    if (!SafeRead(address, current) || current != expected)
    {
        RestoreProtection(page, sizeof(void*), oldProtection);
        return {};
    }
    // Interlocked writes do not fault a shared WRITECOPY image page into a
    // private page on every Windows build. A same-value ordinary write does;
    // the subsequent pointer change is still a single compare-exchange.
    if (!SafeCopy(reinterpret_cast<void*>(address), &expectedPointer,
            sizeof(expectedPointer)))
    {
        RestoreProtection(page, sizeof(void*), oldProtection);
        return {};
    }
    void* observed = InterlockedCompareExchangePointer(destination,
        replacementPointer, expectedPointer);
    if (observed != expectedPointer)
    {
        RestoreProtection(page, sizeof(void*), oldProtection);
        return {false, observed == replacementPointer};
    }
    uintptr_t publishedValue = 0;
    if (RestoreProtection(page, sizeof(void*), oldProtection)
        && SafeRead(address, publishedValue) && publishedValue == replacement)
        return {true, true};

    // The replacement was visible. Roll it back only after writable access is
    // positively re-established; its allocation must be retained either way.
    if (SetWritableProtection(page, sizeof(void*)))
    {
        InterlockedCompareExchangePointer(destination,
            expectedPointer, replacementPointer);
    }
    RestoreProtection(page, sizeof(void*), oldProtection);
    return {false, true};
}

uint64_t PackLuid(const LUID& luid) noexcept
{
    return static_cast<uint64_t>(luid.LowPart)
        | (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32);
}

struct CudaDeviceAPI
{
    using CuInit = int (WINAPI*)(unsigned int);
    using CuDeviceGetCount = int (WINAPI*)(int*);
    using CuDeviceGet = int (WINAPI*)(int*, int);
    using CuDeviceComputeCapability = int (WINAPI*)(int*, int*, int);
    using CuDeviceGetLuid = int (WINAPI*)(char*, unsigned int*, int);
    CuInit initialize{};
    CuDeviceGetCount getCount{};
    CuDeviceGet getDevice{};
    CuDeviceComputeCapability getCapability{};
    CuDeviceGetLuid getLuid{};
};

AdapterKind QueryAdapterKind(const LUID& activeLuid, const CudaDeviceAPI& api,
    int& major, int& minor) noexcept
{
    major = minor = 0;
    int count = 0;
    if (!api.initialize || !api.getCount || !api.getDevice || !api.getCapability
        || !api.getLuid || api.initialize(0) != 0 || api.getCount(&count) != 0 || count <= 0)
        return AdapterKind::Unavailable;
    uint32_t matches = 0;
    int matchedMajor = 0;
    int matchedMinor = 0;
    for (int ordinal = 0; ordinal < count; ++ordinal)
    {
        int device = 0;
        int candidateMajor = 0;
        int candidateMinor = 0;
        std::array<char, sizeof(LUID)> rawLuid{};
        unsigned int nodeMask = 0;
        if (api.getDevice(&device, ordinal) != 0
            || api.getCapability(&candidateMajor, &candidateMinor, device) != 0
            || api.getLuid(rawLuid.data(), &nodeMask, device) != 0)
            return AdapterKind::Unavailable;
        LUID candidateLuid{};
        std::memcpy(&candidateLuid, rawLuid.data(), sizeof(candidateLuid));
        if (candidateLuid.LowPart == activeLuid.LowPart
            && candidateLuid.HighPart == activeLuid.HighPart)
        {
            ++matches;
            matchedMajor = candidateMajor;
            matchedMinor = candidateMinor;
        }
    }
    if (matches != 1 || matchedMajor <= 0 || matchedMinor < 0)
        return AdapterKind::Unavailable;
    major = matchedMajor;
    minor = matchedMinor;
    if (major == 8 && minor == 9) return AdapterKind::Ada;
    return major == 8 && minor == 6 ? AdapterKind::Ampere : AdapterKind::Other;
}

AdapterKind IdentifyAdapter(const LUID& activeLuid, int& major, int& minor) noexcept
{
    HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda)
        return AdapterKind::Unavailable;
    const CudaDeviceAPI api{
        reinterpret_cast<CudaDeviceAPI::CuInit>(GetProcAddress(cuda, "cuInit")),
        reinterpret_cast<CudaDeviceAPI::CuDeviceGetCount>(GetProcAddress(cuda, "cuDeviceGetCount")),
        reinterpret_cast<CudaDeviceAPI::CuDeviceGet>(GetProcAddress(cuda, "cuDeviceGet")),
        reinterpret_cast<CudaDeviceAPI::CuDeviceComputeCapability>(GetProcAddress(cuda, "cuDeviceComputeCapability")),
        reinterpret_cast<CudaDeviceAPI::CuDeviceGetLuid>(GetProcAddress(cuda, "cuDeviceGetLuid"))
    };
    const auto result = QueryAdapterKind(activeLuid, api, major, minor);
    FreeLibrary(cuda);
    return result;
}

void SetFailure(Failure failure) noexcept
{
    gFailure.store(static_cast<uint32_t>(failure), std::memory_order_release);
}
}

void SetLogCallback(LogCallback callback) noexcept
{
    gLogCallback.store(callback, std::memory_order_release);
}

bool BuildAmpereTemporalClone(HMODULE module, AmpereTemporalClone& output) noexcept
{
    output = {};
    uint32_t imageSize = 0;
    if (!ImageSize(module, imageSize)) return false;
    const auto base = reinterpret_cast<uintptr_t>(module);
    if (base > UINTPTR_MAX - imageSize) return false;
    const auto source = FindDescriptorEntry(module, imageSize);
    if (!source.entry || !source.profile) return false;
    const auto& profile = *source.profile;
    uintptr_t descriptor = 0, originalFatbin = 0;
    std::array<uint8_t, kDescriptorBytes> original{};
    if (!SafeRead(source.entry, descriptor) || descriptor < base || descriptor > base + imageSize - kDescriptorBytes ||
        !SafeCopy(original.data(), reinterpret_cast<void*>(descriptor), original.size())) return false;
    std::memcpy(&originalFatbin, original.data() + 8, sizeof(originalFatbin));
    if (originalFatbin < base || profile.sourceFatbinBytes > imageSize || originalFatbin > base + imageSize - profile.sourceFatbinBytes) return false;
    const size_t bytes = kDescriptorBytes + kOutputCapacity + kScratchCapacity;
    auto* allocation = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!allocation) return false;
    const auto fail = [&]() { VirtualFree(allocation, 0, MEM_RELEASE); return false; };
    auto* fatbin = allocation + kDescriptorBytes;
    auto* scratch = fatbin + kOutputCapacity;
    if (!SafeCopy(fatbin, reinterpret_cast<void*>(originalFatbin), profile.sourceFatbinBytes) ||
        !Sha256Equals(fatbin, profile.sourceFatbinBytes, profile.sourceFatbinSha256)) return fail();
    uint32_t outputBytes{}; Failure failure{};
    if (!BuildTemporalFatbin(fatbin, scratch, profile, outputBytes, failure) ||
        !Sha256Equals(fatbin, outputBytes, profile.outputFatbinSha256)) return fail();
    // The accepted Ada output is immutable evidence for the interpolation
    // correction. Change only its target, before exposing the clone anywhere.
    const size_t entry = 16 + profile.sm120EntryBytes;
    constexpr char target[] = ".target sm_89";
    constexpr size_t headerBytes = 104;
    const size_t payload = entry + headerBytes;
    if (payload > outputBytes || ReadU32(fatbin + entry + 28) != 89 ||
        ReadU64(fatbin + entry + 40) != 0x41) return fail();
    const auto targetAt = FindUniqueBytes(fatbin + payload, outputBytes - payload, target, sizeof(target) - 1);
    if (targetAt == SIZE_MAX) return fail();
    fatbin[payload + targetAt + sizeof(target) - 2] = '6';
    const uint32_t sm86 = 86;
    std::memcpy(fatbin + entry + 28, &sm86, sizeof(sm86));
    std::memcpy(allocation, original.data(), original.size());
    const auto replacementFatbin = reinterpret_cast<uintptr_t>(fatbin);
    std::memcpy(allocation + 8, &replacementFatbin, sizeof(replacementFatbin));
    std::memcpy(allocation + 16, &outputBytes, sizeof(outputBytes));
    DWORD previous{};
    if (!VirtualProtect(allocation, bytes, PAGE_READONLY, &previous)) return fail();
    output = {allocation, source.entry, descriptor, reinterpret_cast<uintptr_t>(allocation), outputBytes};
    return true;
}

AdapterKind ObserveD3D12Adapter(void* device) noexcept
{
    if (!device)
    {
        std::lock_guard lock(gMutex);
        gAdapterVerified.store(false, std::memory_order_release);
        gReady.store(false, std::memory_order_release);
        SetFailure(Failure::eAdapterUnavailable);
        return AdapterKind::Unavailable;
    }
    ID3D12Device* d3d12 = nullptr;
    if (FAILED(reinterpret_cast<IUnknown*>(device)->QueryInterface(
            __uuidof(ID3D12Device), reinterpret_cast<void**>(&d3d12))))
    {
        std::lock_guard lock(gMutex);
        gAdapterVerified.store(false, std::memory_order_release);
        gReady.store(false, std::memory_order_release);
        SetFailure(Failure::eAdapterUnavailable);
        return AdapterKind::Unavailable;
    }
    const LUID luid = d3d12->GetAdapterLuid();
    d3d12->Release();
    int major = 0;
    int minor = 0;
    const auto kind = IdentifyAdapter(luid, major, minor);
    const bool verified = kind == AdapterKind::Ada;
    const uint64_t packedLuid = PackLuid(luid);
    {
        std::lock_guard lock(gMutex);
        if (gProvider && gPublishedAdapterLuid != packedLuid)
        {
            gAdapterVerified.store(false, std::memory_order_release);
            gReady.store(false, std::memory_order_release);
            SetFailure(Failure::eRestartRequired);
            Log(L"D157 midpoint fix requires restart after an adapter change");
            return AdapterKind::Unavailable;
        }
        gAdapterLuid.store(packedLuid, std::memory_order_release);
        gAdapterVerified.store(verified, std::memory_order_release);
        if (!verified)
        {
            gReady.store(false, std::memory_order_release);
            SetFailure(kind == AdapterKind::Unavailable
                    ? Failure::eAdapterUnavailable : Failure::eAdapterNotAda);
        }
    }
    Log(L"D157 adapter verification: luid=0x%016llX capability=%d.%d verified=%d",
        static_cast<unsigned long long>(packedLuid), major, minor, verified);
    return kind;
}

bool ObserveD3D12Device(void* device) noexcept
{
    return ObserveD3D12Adapter(device) == AdapterKind::Ada;
}

bool PatchProvider(HMODULE module, const wchar_t* suppliedPath) noexcept
{
    if (!module || !gAdapterVerified.load(std::memory_order_acquire))
        return false;
    std::lock_guard lock(gMutex);
    const uint64_t adapterLuid = gAdapterLuid.load(std::memory_order_acquire);
    if (!gAdapterVerified.load(std::memory_order_acquire) || !adapterLuid)
        return false;
    if (gProvider)
    {
        uintptr_t observed = 0;
        const bool current = gProvider == module
            && gPublishedAdapterLuid == adapterLuid
            && SafeRead(gDescriptorEntry, observed)
            && observed == gReplacementDescriptor
            && IsReadOnlyImageAddress(module, gDescriptorEntry);
        gReady.store(current, std::memory_order_release);
        if (!current)
            SetFailure(Failure::eRestartRequired);
        return current;
    }

    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(module), &pinned))
    {
        SetFailure(Failure::eProviderLayout);
        return false;
    }
    wchar_t loadedPath[32768]{};
    const DWORD pathLength = GetModuleFileNameW(
        pinned, loadedPath, _countof(loadedPath));
    const wchar_t* path = pathLength > 0 && pathLength < _countof(loadedPath)
        ? loadedPath : suppliedPath;
    auto fail = [&](Failure failure) noexcept {
        SetFailure(failure);
        FreeLibrary(pinned);
        Log(L"D157 midpoint fix rejected provider: failure=%u path=%s",
            static_cast<unsigned>(failure), path && *path ? path : L"(unknown)");
        return false;
    };
    if (!dlssg_provider_policy::IsDlssgImplementationModule(pinned))
        return fail(Failure::eProviderIdentity);
    uint32_t imageSize = 0;
    if (!ImageSize(module, imageSize))
        return fail(Failure::eProviderLayout);
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    if (base > UINTPTR_MAX - imageSize)
        return fail(Failure::eProviderLayout);
    const uintptr_t end = base + imageSize;
    const TemporalSource source = FindDescriptorEntry(module, imageSize);
    if (!source.entry || !source.profile || !IsReadOnlyImageAddress(module, source.entry))
        return fail(Failure::eProviderLayout);
    const uintptr_t descriptorEntry = source.entry;
    const auto& profile = *source.profile;

    uintptr_t originalDescriptor = 0;
    std::array<uint8_t, kDescriptorBytes> descriptor{};
    if (!SafeRead(descriptorEntry, originalDescriptor)
        || originalDescriptor < base || originalDescriptor > end - kDescriptorBytes
        || !SafeCopy(descriptor.data(),
            reinterpret_cast<const void*>(originalDescriptor), descriptor.size()))
        return fail(Failure::eProviderLayout);
    uintptr_t originalFatbin = 0;
    uint32_t suppliedSize = 0;
    std::memcpy(&originalFatbin, descriptor.data() + 0x08,
        sizeof(originalFatbin));
    std::memcpy(&suppliedSize, descriptor.data() + 0x10,
        sizeof(suppliedSize));
    if (suppliedSize == 0)
        return fail(Failure::eProviderNotReady);
    if (profile.sourceFatbinBytes > imageSize || suppliedSize != profile.sourceFatbinBytes || originalFatbin < base
        || originalFatbin > end - profile.sourceFatbinBytes)
        return fail(Failure::eSourceIdentity);

    const size_t allocationSize = kDescriptorBytes
        + kOutputCapacity + kScratchCapacity;
    uint8_t* allocation = static_cast<uint8_t*>(VirtualAlloc(nullptr,
        allocationSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!allocation)
        return fail(Failure::eAllocation);
    uint8_t* clonedDescriptor = allocation;
    uint8_t* clonedFatbin = clonedDescriptor + kDescriptorBytes;
    uint8_t* scratch = clonedFatbin + kOutputCapacity;
    std::memcpy(clonedDescriptor, descriptor.data(), descriptor.size());
    if (!SafeCopy(clonedFatbin, reinterpret_cast<const void*>(originalFatbin),
            profile.sourceFatbinBytes)
        || !Sha256Equals(clonedFatbin, profile.sourceFatbinBytes,
            profile.sourceFatbinSha256))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(Failure::eSourceIdentity);
    }
    uint32_t outputBytes = 0;
    Failure transformFailure = Failure::eNone;
    if (!BuildTemporalFatbin(clonedFatbin, scratch, profile, outputBytes,
            transformFailure))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(transformFailure);
    }
    if (!Sha256Equals(clonedFatbin, outputBytes, profile.outputFatbinSha256))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(Failure::eOutputIdentity);
    }
    const uintptr_t clonedFatbinAddress =
        reinterpret_cast<uintptr_t>(clonedFatbin);
    const uintptr_t clonedDescriptorAddress =
        reinterpret_cast<uintptr_t>(clonedDescriptor);
    std::memcpy(clonedDescriptor + 0x08, &clonedFatbinAddress,
        sizeof(clonedFatbinAddress));
    std::memcpy(clonedDescriptor + 0x10, &outputBytes, sizeof(outputBytes));
    DWORD oldProtection = 0;
    if (!VirtualProtect(allocation, allocationSize, PAGE_READONLY,
            &oldProtection))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(Failure::ePublication);
    }
    if (!gAdapterVerified.load(std::memory_order_acquire)
        || gAdapterLuid.load(std::memory_order_acquire) != adapterLuid)
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(Failure::eRestartRequired);
    }

    const PublishResult published = PublishPointer(descriptorEntry,
        originalDescriptor, clonedDescriptorAddress);
    if (!published.success)
    {
        if (!published.replacementWasVisible)
            VirtualFree(allocation, 0, MEM_RELEASE);
        else
        {
            // A concurrent reader may retain the briefly visible clone.
            gPinnedProvider = pinned;
            pinned = nullptr;
        }
        if (pinned)
            FreeLibrary(pinned);
        SetFailure(Failure::ePublication);
        Log(L"D157 midpoint fix publication failed: path=%s", path);
        return false;
    }

    gProvider = module;
    gPinnedProvider = pinned;
    gPublishedAdapterLuid = adapterLuid;
    gDescriptorEntry = descriptorEntry;
    gReplacementDescriptor = clonedDescriptorAddress;
    gAllocation = allocation;
    gReady.store(true, std::memory_order_release);
    SetFailure(Failure::eNone);
    Log(L"D157 midpoint fix published at provider RVA 0x%zX: %s",
        static_cast<size_t>(descriptorEntry - base), path);
    return true;
}

bool AdapterVerified() noexcept
{
    return gAdapterVerified.load(std::memory_order_acquire);
}

bool Ready() noexcept
{
    return gReady.load(std::memory_order_acquire);
}

uint32_t FailureCode() noexcept
{
    return gFailure.load(std::memory_order_acquire);
}

}
