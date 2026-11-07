#pragma once
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace TheosRenderPipeline::AMDAdapter {
struct PCI {
    unsigned domain{}, bus{}, device{}, function{};
    bool operator==(const PCI& other) const {
        return domain==other.domain && bus==other.bus && device==other.device && function==other.function;
    }
};
inline bool ParsePCI(std::string_view text, PCI& result) {
    PCI value;
    unsigned* fields[]{&value.domain,&value.bus,&value.device,&value.function};
    const char separators[]{':',':','.', '\0'};
    for (unsigned i=0;i<4;++i) {
        const auto end=i==3 ? text.size() : text.find(separators[i]);
        if (end==std::string_view::npos || end==0) return false;
        const auto parsed=std::from_chars(text.data(),text.data()+end,*fields[i],16);
        if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+end) return false;
        text.remove_prefix(end+(i==3 ? 0 : 1));
    }
    if (!text.empty() || value.domain>0xffff || value.bus>0xff || value.device>31 || value.function>7) return false;
    result=value; return true;
}
struct Identity {
    int ordinal{};
    std::uint64_t luid{};
    PCI pci{};
    bool luidKnown{}, pciKnown{};
};
enum class Result { NoMatch, Selected, Ambiguous };
struct Selection {
    Result result{Result::NoMatch};
    int ordinal{-1};
    bool byLUID{};
};
inline Selection Select(const Identity* devices, std::size_t count,
                        std::uint64_t expectedLUID, const PCI* expectedPCI) {
    Selection result;
    if (expectedLUID) {
        for (std::size_t i=0;i<count;++i) {
            const auto& candidate=devices[i];
            if (!candidate.luidKnown || candidate.luid!=expectedLUID) continue;
            if (result.result==Result::Selected) return {Result::Ambiguous,-1,false};
            result={Result::Selected,candidate.ordinal,true};
        }
        if (result.result==Result::Selected) return result;
    }
    for (std::size_t i=0;i<count;++i) {
        const auto& candidate=devices[i];
        // An available LUID is authoritative. A contradictory LUID must not
        // fall through to PCI matching. Never infer identity from GPU ordinal.
        const bool match=!(candidate.luidKnown && expectedLUID) &&
            candidate.pciKnown && expectedPCI && candidate.pci==*expectedPCI;
        if (!match) continue;
        if (result.result==Result::Selected) return {Result::Ambiguous,-1,false};
        result={Result::Selected,candidate.ordinal,false};
    }
    return result;
}
}
