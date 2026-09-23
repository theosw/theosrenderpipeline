#pragma once
#include "ptx_retarget.hpp"
#include "../MFGTuring/ptx_retarget.hpp"

namespace trp::ampere {
// Keep the previously tested Ampere planner byte-for-byte unchanged.
inline Status RetargetForTarget(const std::uint8_t* bytes, std::size_t size,
                               Plan& plan, std::string& reason, std::uint32_t target) {
    if (target == 86) return Retarget(bytes, size, plan, reason);
    plan = {}; reason.clear();
    if (target != 75) { reason = "unsupported provider target"; return Status::Rejected; }
    mfgunlock::ptx::Plan converted;
    const auto result = mfgunlock::ptx::Retarget({bytes, size}, converted, reason,
                                               mfgunlock::architecture::kTuring);
    if (result == mfgunlock::ptx::Result::kRejected) return Status::Rejected;
    if (result == mfgunlock::ptx::Result::kUnchanged) return Status::Unchanged;
    plan = {std::move(converted.replacement), converted.ptx_offset,
            converted.ptx_bytes, converted.hidden_cubins};
    return Status::Retargeted;
}
}
