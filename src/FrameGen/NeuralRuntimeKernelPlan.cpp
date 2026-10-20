#include "NeuralRuntimeKernelsInternal.h"
#include "NeuralRuntimeKernels.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <utility>

namespace TheosRenderPipeline::NeuralRendering::KernelPacks
{
    namespace
    {
        template<class T> T Value(std::span<const std::uint8_t> bytes, std::size_t offset)
        {
            T value{};
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
            return value;
        }
        template<class T> std::vector<std::uint8_t> Bytes(T value)
        {
            std::vector<std::uint8_t> bytes(sizeof(T));
            std::memcpy(bytes.data(), &value, sizeof(T));
            return bytes;
        }
        bool Match(Memory& memory, const Patch& patch, bool replacement)
        {
            std::array<std::uint8_t, 8> actual{};
            const auto& wanted = replacement ? patch.replacement : patch.original;
            return wanted.size() <= actual.size() && memory.Read(patch.rva, std::span(actual).first(wanted.size())) &&
                std::equal(wanted.begin(), wanted.end(), actual.begin());
        }
    }

    bool MakePlan(std::span<const std::uint8_t> pack, std::uintptr_t module,
        std::uintptr_t storage, std::vector<Patch>& plan, std::string& reason,
        std::size_t packSize, bool admitGtx10)
    {
        plan.clear();
        const auto refuse = [&](const char* why) { reason = why; return false; };
        if (pack.size() != packSize || std::memcmp(pack.data(), "TRPNRK1\0", 8) ||
            Value<std::uint32_t>(pack, 8) != 1 || Value<std::uint32_t>(pack, 12) != std::size(Bundles)) {
            return refuse("invalid NR compatibility kernel pack header or size");
        }
        constexpr char hex[] = "0123456789ABCDEF";
        for (std::size_t i = 0; i < 32; ++i) {
            if (hex[pack[16+i] >> 4] != kKernelRuntimeSha256[2*i] ||
                hex[pack[16+i] & 15] != kKernelRuntimeSha256[2*i+1]) {
                return refuse("NR compatibility kernel pack targets another runtime");
            }
        }
        if (!module || !storage || module > INTPTR_MAX - ImageSize || storage > INTPTR_MAX - pack.size() ||
            (module < storage+pack.size() && storage < module+ImageSize)) {
            return refuse("invalid module or pack address");
        }
        std::vector<Patch> pending;
        pending.reserve(38);
        std::uint64_t end = 512; // header + 15 records, aligned to 256
        for (std::size_t i = 0; i < std::size(Bundles); ++i) {
            const auto& b = Bundles[i];
            const auto at = 48 + i * 24;
            const auto offset = Value<std::uint64_t>(pack, at+8), size = Value<std::uint64_t>(pack, at+16);
            if (Value<std::uint32_t>(pack, at) != b.original || Value<std::uint32_t>(pack, at+4) != b.bytes ||
                offset != ((end+255) & ~std::uint64_t(255)) || size < 16 || offset > pack.size() || size > pack.size()-offset) {
                return refuse("NR bundle table range or source identity differs");
            }
            constexpr std::array<std::uint8_t, 8> magic{0x50,0xED,0x55,0xBA,1,0,0x10,0};
            if (!std::equal(magic.begin(), magic.end(), pack.begin()+offset) ||
                Value<std::uint64_t>(pack, static_cast<std::size_t>(offset)+8) != size-16) {
                return refuse("NR bundle is not a complete fatbin");
            }
            end = offset + size;
            const auto target = storage + offset;
            if (b.pointer) {
                pending.push_back({b.site, Bytes<std::uint64_t>(module+b.original), Bytes<std::uint64_t>(target)});
                // CRT initializer 0x73D0 copies these two DWORD lengths into
                // descriptors before LoadLibrary returns. Replacing only the
                // source scalar leaves the driver hashing the old byte count.
                pending.push_back({b.site+8, Bytes<std::uint32_t>(b.bytes), Bytes<std::uint32_t>(static_cast<std::uint32_t>(size))});
            } else {
                const auto next = module + b.site + 7;
                const auto distance = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next);
                if (distance < INT32_MIN || distance > INT32_MAX) { return refuse("NR bundle is outside RIP-relative reach"); }
                std::vector<std::uint8_t> original{static_cast<std::uint8_t>(i < 7 ? 0x4C : 0x48),0x8D,0x05};
                const auto displacement = Bytes<std::int32_t>(static_cast<std::int32_t>(b.original-b.site-7));
                original.insert(original.end(), displacement.begin(), displacement.end());
                auto replacement = original;
                const auto relative = static_cast<std::int32_t>(distance);
                std::memcpy(replacement.data()+3, &relative, 4);
                pending.push_back({b.site, std::move(original), std::move(replacement)});
            }
            pending.push_back({b.length, Bytes<std::uint64_t>(b.bytes), Bytes<std::uint64_t>(size)});
        }
        if (end != pack.size()) { return refuse("NR compatibility kernel pack has trailing bytes"); }
        for (const auto site : {0x136E0u, 0x15580u}) {
            pending.push_back({site, {0xC7,0x44,0x24,0x3C,0xB0,1,0,0},
                {0xC7,0x44,0x24,0x3C,static_cast<std::uint8_t>(admitGtx10 ? 0x30 : 0x60),1,0,0}});
        }
        if (admitGtx10) {
            // Unlisted architectures continue when >= this compare: 0x140 -> 0x130 (GTX 10).
            pending.push_back({0x17BEF, {0x3D,0x40,1,0,0}, {0x3D,0x30,1,0,0}});
        }
        // Same audited host admission as the pinned community runtime. Each
        // architecture entry is verified independently before publication.
        constexpr std::pair<std::uint32_t, std::uint32_t> architectureSites[]{
            {0x18144, 0x17BCC}, {0x18148, 0x17BD3}, {0x1814C, 0x17BE1}, {0x18150, 0x17BDA}};
        for (const auto [site, original] : architectureSites) {
            pending.push_back({site, Bytes<std::uint32_t>(original), Bytes<std::uint32_t>(0x17C2B)});
        }
        plan = std::move(pending);
        return true;
    }

    bool Matches(Memory& memory, std::span<const Patch> patches, bool replacement)
    {
        for (const auto& p : patches) { if (!Match(memory, p, replacement)) { return false; } }
        return true;
    }

    Publication Publish(Memory& memory, std::span<const Patch> patches, std::string& reason)
    {
        struct Page { DWORD old{}; bool writable{}; };
        std::map<std::uint32_t, Page> pages;
        if (patches.empty()) { reason = "empty patch transaction"; return Publication::Refused; }
        for (std::size_t i = 0; i < patches.size(); ++i) {
            const auto& p = patches[i];
            if (p.original.empty() || p.original.size() > 8 || p.original.size() != p.replacement.size() ||
                p.rva > ImageSize || p.original.size() > ImageSize-p.rva) {
                reason = "invalid patch range"; return Publication::Refused;
            }
            for (std::size_t j = 0; j < i; ++j) {
                const auto& other = patches[j];
                if (p.rva < other.rva+other.original.size() && other.rva < p.rva+p.original.size()) {
                    reason = "overlapping patch ranges"; return Publication::Refused;
                }
            }
            for (auto page = p.rva & ~(PageSize-1); page <= ((p.rva+static_cast<std::uint32_t>(p.original.size())-1) & ~(PageSize-1)); page += PageSize) {
                pages.try_emplace(page);
            }
        }
        if (!Matches(memory, patches, false)) { reason = "original runtime bytes differ"; return Publication::Refused; }
        // All allocations finish before changing protection or writing bytes.
        reason.reserve(128);
        const auto restore = [&]() {
            bool ok = true;
            for (auto it = pages.rbegin(); it != pages.rend(); ++it) {
                if (!it->second.writable) { continue; }
                DWORD ignored{};
                const bool restored = memory.Protect(it->first, it->second.old, ignored) && memory.ProtectionIs(it->first, it->second.old);
                if (restored) { it->second.writable = false; }
                ok = restored && ok;
            }
            return ok;
        };
        for (auto& [page, state] : pages) {
            if (!memory.Protect(page, PAGE_EXECUTE_READWRITE, state.old)) {
                const bool restored = restore();
                reason = "runtime page protection refused";
                return restored && Matches(memory, patches, false) ? Publication::Refused : Publication::Unsafe;
            }
            state.writable = true;
        }
        if (!Matches(memory, patches, false)) {
            restore();
            reason = "runtime bytes changed during admission";
            return Publication::Unsafe; // Never overwrite another owner's edit.
        }
        bool wrote = true;
        for (const auto& p : patches) { if (!memory.Write(p.rva, p.replacement)) { wrote = false; break; } }
        const bool verified = wrote && Matches(memory, patches, true);
        const bool flushed = verified && memory.Flush();
        const bool protectedAgain = restore();
        if (verified && flushed && protectedAgain) { reason = "NR compatibility kernel pack installed in memory"; return Publication::Applied; }
        bool writable = true;
        for (auto& [page, state] : pages) {
            DWORD ignored{};
            const bool acquired = memory.Protect(page, PAGE_EXECUTE_READWRITE, ignored);
            state.writable = acquired || state.writable;
            writable = acquired && writable;
        }
        bool originals = writable;
        if (writable) {
            for (const auto& p : patches) { originals = memory.Write(p.rva, p.original) && originals; }
            originals = Matches(memory, patches, false) && originals;
        }
        const bool rollbackFlushed = memory.Flush();
        const bool rollbackProtected = restore();
        const bool clean = originals && rollbackFlushed && rollbackProtected;
        reason = clean ? "NR installation failed; original runtime restored" : "NR rollback unconfirmed; relaunch required";
        return clean ? Publication::Refused : Publication::Unsafe;
    }
}
