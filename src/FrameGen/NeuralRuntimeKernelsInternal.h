#pragma once

#include <Windows.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TheosRenderPipeline::NeuralRendering::KernelPacks
{
    inline constexpr std::uint32_t ImageSize = 0xFE2F000;
    inline constexpr std::size_t PackSize = 12215592;
    inline constexpr std::uint32_t PageSize = 4096;
    // A touched image WRITECOPY page is private. Windows may report READWRITE
    // after restoring WRITECOPY; access permissions and modifiers must agree.
    constexpr bool ProtectionEquivalent(DWORD actual, DWORD requested)
    {
        return actual == requested ||
            (requested == PAGE_WRITECOPY && actual == PAGE_READWRITE) ||
            (requested == PAGE_EXECUTE_WRITECOPY && actual == PAGE_EXECUTE_READWRITE);
    }
    struct Bundle {
        std::uint32_t site, length, original, bytes;
        bool pointer;
    };
    inline constexpr Bundle Bundles[]{
        {0x605F3, 0x470540, 0x1106D0, 841720, false},
        {0x6060B, 0x470548, 0x1DDED0, 557032, false},
        {0x60626, 0x470550, 0x265EC0, 506464, false},
        {0x60641, 0x470558, 0x2E1920, 586832, false},
        {0x6065C, 0x470560, 0x370D70, 433184, false},
        {0x60677, 0x470568, 0x3DA990, 542960, false},
        {0x60692, 0x470570, 0x45F280, 70336, false},
        {0x6D528, 0x473090, 0x471140, 8016, false},
        {0x6D53B, 0x475B38, 0x4730A0, 10904, false},
        {0x6D54F, 0x478950, 0x475B40, 11792, false},
        {0x6D563, 0x47B2A0, 0x478960, 10560, false},
        {0x6D577, 0x47D998, 0x47B2B0, 9960, false},
        {0x6D58B, 0x47F338, 0x47D9A0, 6552, false},
        {0x1100C8, 0x48C968, 0x47F340, 54824, true},
        {0x110100, 0x48E488, 0x48C970, 6936, true},
    };
    struct Patch { std::uint32_t rva; std::vector<std::uint8_t> original, replacement; };
    enum class Publication { Refused, Applied, Unsafe };
    struct Memory {
        virtual ~Memory() = default;
        virtual bool Read(std::uint32_t, std::span<std::uint8_t>) = 0;
        virtual bool Write(std::uint32_t, std::span<const std::uint8_t>) = 0;
        virtual bool Protect(std::uint32_t page, DWORD protection, DWORD& old) = 0;
        virtual bool ProtectionIs(std::uint32_t page, DWORD protection) = 0;
        virtual bool Flush() = 0;
    };
    // Pure validation and planning; never writes the module or pack. Production
    // additionally requires both pinned SHA-256 identities before calling this.
    // admitGtx10 (research): a pack of any size, and admission down to GTX 10 (0x130).
    bool MakePlan(std::span<const std::uint8_t> pack, std::uintptr_t module,
        std::uintptr_t storage, std::vector<Patch>& plan, std::string& reason,
        std::size_t packSize = PackSize, bool admitGtx10 = false);
    bool Matches(Memory&, std::span<const Patch>, bool replacement);
    // Caller guarantees exclusive ownership before initialization. A transaction
    // here provides verified rollback, not atomic publication to running threads.
    Publication Publish(Memory&, std::span<const Patch>, std::string& reason);
}
