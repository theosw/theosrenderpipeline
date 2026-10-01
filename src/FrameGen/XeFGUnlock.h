// Experimental XeFG compatibility. Exact pinned runtime; opt-in only.
#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TheosRenderPipeline::XeFGUnlock {
enum class PatchResult { Refused, Applied, Unsafe };
struct Patch { std::uint32_t rva; std::vector<std::uint8_t> original, replacement; };
struct Memory {
    virtual ~Memory() = default;
    virtual bool Read(std::uint32_t, std::span<std::uint8_t>) = 0;
    virtual bool Write(std::uint32_t, std::span<const std::uint8_t>) = 0;
    virtual bool Protect(std::uint32_t page, DWORD value, DWORD& old) = 0;
    virtual bool ProtectionIs(std::uint32_t page, DWORD value) = 0;
    virtual bool Flush() = 0;
};
// The caller owns admission and guarantees no concurrent entry into this image.
PatchResult Publish(Memory&, std::span<const Patch>, std::uint32_t imageSize, std::string& reason);
std::vector<Patch> MakePlan(std::uintptr_t present, std::uintptr_t scheduler, std::uintptr_t deadline);
struct Event {
    std::uintptr_t context{}, resource{};
    std::uint64_t burst{}, index{}, count{};
    std::int64_t entryQpc{}, pacedQpc{}, exitQpc{}, result{}, deadlineNs{};
    bool scheduler{}, scheduled{}, finalFrame{};
};
using Observer = void (*)(const Event&) noexcept;
struct Stats { std::uint64_t presents{}, scheduled{}, refused{}, wallWaits{}, deadlines{}, contextChanges{}; };
// Hooks remain resident until all SDK contexts have been destroyed. Never unload
// the module/code while patched. The host permits exactly one active SDK owner.
PatchResult Install(HMODULE runtime, Observer observer, std::string& reason, bool forceSchedulerRefusal=false);
PatchResult EnsureInstalled(HMODULE runtime, std::string& reason);
void NewContextEpoch();
Stats Snapshot();
void SaveEvents(const char* path);
}
