#pragma once
#include <cstdint>
#include <cstddef>

namespace TRPNvapi {
using Query = void* (*)(std::uint32_t);
using GetLuid = int (*)(char*, unsigned*, int);
struct LogicalData {
    std::uint32_t version;
    void* osAdapterId;
    std::uint32_t physicalGpuCount;
    void* physicalGpuHandles[64];
    std::uint32_t reserved[8];
};
struct ArchInfo { std::uint32_t version, architecture, implementation, revision; };
constexpr std::uint32_t LogicalVersion = sizeof(LogicalData) | (1u << 16);
constexpr std::uint32_t ArchVersion = sizeof(ArchInfo) | (1u << 16);
static_assert(offsetof(LogicalData, osAdapterId)==8 && sizeof(LogicalData)==568);
constexpr std::uint32_t LogicalInfoId=0x842b066e, ArchInfoId=0xd8265d24;
}
