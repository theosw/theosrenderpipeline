#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace TheosRenderPipeline::NeuralRendering
{
    struct LaunchDim { std::uint32_t x{}, y{}, z{}; bool operator==(const LaunchDim&) const = default; };
    struct LaunchRequirement {
        std::uint32_t bundle{}, architectures{}, flags{}; // architecture bits: 1=SM61, 2=SM89
        LaunchDim block{}, grid{};
        std::uint32_t sharedBytes{};
        bool operator==(const LaunchRequirement&) const = default;
    };
    using LaunchRequirements = std::map<std::string, LaunchRequirement, std::less<>>;

    inline bool ReadLaunchRequirements(std::span<const std::uint8_t> bytes, LaunchRequirements& rules, std::string& reason)
    {
        rules.clear();
        auto fail = [&] { reason = "invalid NR required-launch metadata"; return false; };
        if (bytes.size() < 16 || std::memcmp(bytes.data(), "TRPLCH1\0", 8)) { return fail(); }
        auto word = [&](std::size_t at) { std::uint32_t v{}; std::memcpy(&v, bytes.data()+at, 4); return v; };
        const auto count = word(12);
        if (word(8) != 1 || !count || count > 1024) { return fail(); }
        std::size_t at = 16;
        LaunchRequirements pending;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (at > bytes.size() || bytes.size()-at < 44) { return fail(); }
            LaunchRequirement r{word(at), word(at+4), word(at+12),
                {word(at+16),word(at+20),word(at+24)}, {word(at+28),word(at+32),word(at+36)},word(at+40)};
            const auto n = word(at+8);
            at += 44;
            if (!n || n > 512 || n > bytes.size()-at || r.bundle >= 15 || !r.architectures || (r.architectures & ~3u) ||
                (r.flags & ~3u) || !r.block.x || !r.block.y || !r.block.z || r.block.x > 1024 || r.block.y > 1024 ||
                r.block.z > 64 || std::uint64_t(r.block.x)*r.block.y*r.block.z > 1024 ||
                ((r.flags & 1) ? (!r.grid.x || r.grid.x > INT32_MAX || !r.grid.y || r.grid.y > 65535 || !r.grid.z || r.grid.z > 65535) :
                    (r.grid.x || r.grid.y || r.grid.z)) || r.sharedBytes > 49152 || (!(r.flags & 2) && r.sharedBytes)) { return fail(); }
            std::string name(reinterpret_cast<const char*>(bytes.data()+at), n);
            for (const auto c : name) { if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) { return fail(); } }
            at += n;
            while (at % 4) { if (at >= bytes.size() || bytes[at++] != 0) { return fail(); } }
            if (!pending.emplace(std::move(name), r).second) { return fail(); }
        }
        if (at != bytes.size()) { return fail(); }
        rules = std::move(pending);
        return true;
    }
}
