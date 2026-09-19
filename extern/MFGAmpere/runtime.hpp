#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <filesystem>
#include <cstdint>

namespace trp::ampere {
struct RuntimeStatus {
    bool prepared{}, bridgeInstalled{}, failed{}, createSeen{};
    std::uint32_t fatbins{}, resolverMask{}, requirementsCalls{}, capabilityCalls{}, createCalls{};
    const char* error{};
};
using Log = void (*)(const char*);
// One host, one adapter, one provider. This owner and its published data remain
// alive until process exit. Call only at the controlled pre-slInit boundary.
// Separate modules are permitted only by the host's coordinated CS path.
bool Start(ID3D12Device* device, const std::filesystem::path& directory, Log log, bool allowSeparateModules = false) noexcept;
RuntimeStatus Snapshot() noexcept;
bool Verify() noexcept;
void EnterStartupScope() noexcept;
void LeaveStartupScope() noexcept;
} // namespace trp::ampere
