#pragma once

#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

namespace TheosRenderPipeline::Compatibility
{
inline std::optional<std::array<std::uint8_t, 32>> Sha256File(const std::filesystem::path& a_path)
{
    std::ifstream input(a_path, std::ios::binary);
    if (!input)
    {
        return std::nullopt;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD hashSize = 0;
    DWORD resultSize = 0;
    std::vector<std::uint8_t> object;
    std::array<std::uint8_t, 32> result{};
    bool success = BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    if (success)
    {
        success =
            BCRYPT_SUCCESS(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                                               sizeof(objectSize), &resultSize, 0));
    }
    if (success)
    {
        success = BCRYPT_SUCCESS(::BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize),
                                                     sizeof(hashSize), &resultSize, 0)) &&
                  hashSize == result.size();
    }
    if (success)
    {
        object.resize(objectSize);
        success = BCRYPT_SUCCESS(
            ::BCryptCreateHash(algorithm, &hash, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0));
    }

    std::array<char, 65536> chunk{};
    while (success && input)
    {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = input.gcount();
        if (count > 0)
        {
            success = BCRYPT_SUCCESS(
                ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()), static_cast<ULONG>(count), 0));
        }
    }
    if (success)
    {
        success = BCRYPT_SUCCESS(::BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0));
    }

    if (hash)
    {
        ::BCryptDestroyHash(hash);
    }
    if (algorithm)
    {
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return success ? std::optional(result) : std::nullopt;
}
} // namespace TheosRenderPipeline::Compatibility
