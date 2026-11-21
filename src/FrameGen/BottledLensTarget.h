#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace TheosRenderPipeline::BottledLens
{
    // Bottled Shaders' CameraFX::GetLensTarget(const SceneTarget&, bool& uiLayer):
    // the one place that decides whether the Camera FX lens overlay is drawn
    // on the scene or, for Bottled's own frame generation, into its UI buffer.
    // The whole 64-byte function as built in Bottled Shaders V2 2.0.1; -1 is a
    // RIP displacement, which moves with every build. Every other byte is fixed,
    // so a match also pins what the code does: clear *uiLayer, test three
    // flags and the UI buffer, and otherwise return the scene target's view
    // from offset 8 of the first argument.
    inline constexpr std::array<std::int16_t, 64> kLensTarget{
        0x41, 0xC6, 0x00, 0x00,                   // mov byte [r8], 0
        0x80, 0x3D, -1, -1, -1, -1, 0x00,         // cmp byte [rip+?], 0
        0x74, 0x2E,                               // je scene
        0x80, 0x3D, -1, -1, -1, -1, 0x00,         // cmp byte [rip+?], 0
        0x74, 0x25,                               // je scene
        0x80, 0x3D, -1, -1, -1, -1, 0x00,         // cmp byte [rip+?], 0
        0x74, 0x1C,                               // je scene
        0x48, 0x8B, 0x05, -1, -1, -1, -1,         // mov rax, [rip+?]
        0x48, 0x85, 0xC0,                         // test rax, rax
        0x74, 0x10,                               // je scene
        0x48, 0x83, 0x78, 0x18, 0x00,             // cmp qword [rax+18h], 0
        0x74, 0x09,                               // je scene
        0x41, 0xC6, 0x00, 0x01,                   // mov byte [r8], 1
        0x48, 0x8B, 0x40, 0x18,                   // mov rax, [rax+18h]
        0xC3,                                     // ret
        0x48, 0x8B, 0x42, 0x08,                   // scene: mov rax, [rdx+8]
        0xC3 };                                   // ret

    inline bool Matches(std::span<const std::uint8_t> code)
    {
        if (code.size() < kLensTarget.size()) { return false; }
        for (std::size_t i = 0; i < kLensTarget.size(); ++i) {
            if (kLensTarget[i] >= 0 && code[i] != static_cast<std::uint8_t>(kLensTarget[i])) { return false; }
        }
        return true;
    }

    struct Search
    {
        std::size_t matches{};
        std::size_t offset{}; // Of the first match; meaningful only when matches == 1.
    };

    // Every occurrence in one executable range. A build is supported only
    // when the function appears exactly once across all of them.
    inline Search Find(std::span<const std::uint8_t> code)
    {
        Search result;
        if (code.size() < kLensTarget.size()) { return result; }
        for (std::size_t i = 0; i + kLensTarget.size() <= code.size(); ++i) {
            // The first six bytes are fixed; most positions fail on the first.
            if (code[i] != 0x41 || code[i + 1] != 0xC6) { continue; }
            if (!Matches(code.subspan(i))) { continue; }
            if (result.matches++ == 0) { result.offset = i; }
        }
        return result;
    }

    // Direct calls and jumps (E8/E9 rel32) in one executable range that land
    // on target. base and target are offsets from the same origin; base is
    // where code starts. The signature pins the function, not what its caller
    // does with the answer, so a build is supported only when the function has
    // the single direct call the checked build has, and nothing jumps to it.
    struct References
    {
        std::size_t calls{}, jumps{};
    };
    inline References FindReferences(std::span<const std::uint8_t> code, std::size_t base, std::size_t target)
    {
        References result;
        for (std::size_t i = 0; i + 5 <= code.size(); ++i) {
            if (code[i] != 0xE8 && code[i] != 0xE9) { continue; }
            const auto relative = static_cast<std::int32_t>(std::uint32_t{code[i + 1]} | std::uint32_t{code[i + 2]} << 8 |
                std::uint32_t{code[i + 3]} << 16 | std::uint32_t{code[i + 4]} << 24);
            if (static_cast<std::int64_t>(base + i + 5) + relative != static_cast<std::int64_t>(target)) { continue; }
            ++(code[i] == 0xE8 ? result.calls : result.jumps);
        }
        return result;
    }
}
