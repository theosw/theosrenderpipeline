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
    struct LaunchBound {
        std::uint32_t maxThreads{};
        LaunchDim requiredBlock{};
        std::uint32_t maxDynamicShared{};
        bool operator==(const LaunchBound&) const = default;
    };
    struct LaunchRequirement {
        std::uint32_t bundle{}, architectures{}, flags{}; // architecture bits: 1=SM61, 2=SM89
        // flags: 1=fixed grid, 2=fixed shared bytes, 4=grid multipliers (exclusive with 1).
        LaunchDim block{}, grid{};
        std::uint32_t sharedBytes{};
        std::array<LaunchBound,2> bounds{}; // SM61, SM89; maxThreads=0 means unbounded (legacy).
        bool operator==(const LaunchRequirement&) const = default;
    };
    using LaunchRequirements = std::map<std::string, LaunchRequirement, std::less<>>;

    inline bool ReadLaunchRequirements(std::span<const std::uint8_t> bytes, LaunchRequirements& rules, std::string& reason)
    {
        rules.clear();
        auto fail = [&] { reason = "invalid NR required-launch metadata"; return false; };
        if (bytes.size() < 16 || std::memcmp(bytes.data(), "TRPLCH1\0", 8)) { return fail(); }
        auto word = [&](std::size_t at) { std::uint32_t v{}; std::memcpy(&v, bytes.data()+at, 4); return v; };
        const auto count = word(12), schema = word(8);
        if ((schema != 1 && schema != 2) || !count || count > 1024) { return fail(); }
        std::size_t at = 16;
        LaunchRequirements pending;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t recordBytes = schema == 2 ? 84 : 44;
            if (at > bytes.size() || bytes.size()-at < recordBytes) { return fail(); }
            LaunchRequirement r{word(at), word(at+4), word(at+12),
                {word(at+16),word(at+20),word(at+24)}, {word(at+28),word(at+32),word(at+36)},word(at+40)};
            const auto n = word(at+8);
            if (schema == 2) {
                for (unsigned arch = 0; arch < 2; ++arch) {
                    const auto b = at+44+arch*20;
                    auto& bound = r.bounds[arch];
                    bound = {word(b), {word(b+4),word(b+8),word(b+12)},word(b+16)};
                    const auto& d = bound.requiredBlock;
                    if (bound.maxThreads > 1024 || bound.maxDynamicShared > 49152 ||
                        (!bound.maxThreads && (d.x || d.y || d.z || bound.maxDynamicShared)) ||
                        ((d.x || d.y || d.z) && (!d.x || !d.y || !d.z || d.x > 1024 || d.y > 1024 || d.z > 64 ||
                            std::uint64_t(d.x)*d.y*d.z > bound.maxThreads))) { return fail(); }
                }
            }
            const bool boundOnly = schema == 2 && !r.block.x && !r.block.y && !r.block.z &&
                !r.flags && (r.bounds[0].maxThreads || r.bounds[1].maxThreads);
            at += recordBytes;
            if (!n || n > 512 || n > bytes.size()-at || r.bundle >= 15 || !r.architectures || (r.architectures & ~3u) ||
                (r.flags & ~7u) || ((r.flags & 5u) == 5u) || (!boundOnly && (!r.block.x || !r.block.y || !r.block.z || r.block.x > 1024 || r.block.y > 1024 ||
                r.block.z > 64 || std::uint64_t(r.block.x)*r.block.y*r.block.z > 1024)) ||
                ((r.flags & 4) ? (!r.grid.x || r.grid.x > 64 || !r.grid.y || r.grid.y > 64 || !r.grid.z || r.grid.z > 64) :
                (r.flags & 1) ? (!r.grid.x || r.grid.x > INT32_MAX || !r.grid.y || r.grid.y > 65535 || !r.grid.z || r.grid.z > 65535) :
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
