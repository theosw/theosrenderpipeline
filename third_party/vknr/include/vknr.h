#pragma once
// Private TRP ABI. NVIDIA's DLL is read as data only. No command-list ABI.
#include <windows.h>
#include <d3d12.h>
#include <cstdint>

namespace vknr {
inline constexpr std::uint32_t Version = 3;
inline constexpr std::uint32_t Slots = 3;
enum class Result : std::uint32_t { Ok, Busy, Refused, Failed };
enum class Path : std::uint32_t { Portable, RDNA3 };
struct Capability {
    std::uint32_t size{sizeof(Capability)}, version{Version};
    bool available{}, rdna3{};
    Path path{Path::Portable};
    char revision[48]{}, device[256]{}, reason[512]{}, pathReason[256]{};
};
struct SharedImage { HANDLE handle{}; std::uint32_t width{}, height{}; DXGI_FORMAT format{}; };
struct Create {
    std::uint32_t size{sizeof(Create)}, version{Version};
    ID3D12Device* device{};
    const wchar_t* weightsPath{};
    const wchar_t* shaderRoot{};
    const wchar_t* cacheRoot{};
    SharedImage images[4]; // colour RGBA16F, motion RG32F, depth R32F, output RGBA16F
    HANDLE fence{}; // shared D3D12 timeline; caller retains resources through release
    bool validation{}; // debug/test build only; never registers a layer
};
struct Frame {
    std::uint32_t size{sizeof(Frame)}, slot{};
    std::uint64_t ready{}, done{};
    float motionScaleX{1}, motionScaleY{1}; // normalized screen displacement
    float localTone{1}, localStructure{1}, skin{1};
    std::int32_t style{};
    bool autoSkin{}, reset{}, depthInverted{};
};
using Handle = void*;
// The callback receives complete JSON diagnostics while the query owns them.
// No fixed-size tuple buffer: every advertised cooperative tuple is reported.
using Diagnostics = void(__cdecl*)(const char*);
using Query = Result(__cdecl*)(ID3D12Device*, const wchar_t*, Capability*, Diagnostics);
// Failed creation can return a nonnull retained handle when GPU retirement is
// uncertain. The host must retain its shared resources with that handle.
using CreateFn = Result(__cdecl*)(const Create*, Handle*, Capability*);
// Prepare is nonblocking, records into an engine-owned retired slot, and does not submit.
// A failed/abandoned prepared frame invalidates this session; release/recreate it.
using Prepare = Result(__cdecl*)(Handle, const Frame*);
// Host: submit producer, Signal(ready), Submit, Wait(done), record/submit consumer.
// Never wait for done unless Submit returned Ok. All calls are serialized by TRP.
using Submit = Result(__cdecl*)(Handle);
// Polls actual Vulkan fences, never a submission-count heuristic. Busy retains EVERYTHING.
// Host must first retire D3D12 consumers, including failed partial transactions.
using Release = Result(__cdecl*)(Handle);
using LastError = const char*(__cdecl*)(Handle);
using Timing = double(__cdecl*)(Handle); // most recent completed network, ms; -1 pending
struct Completion { std::uint32_t size{sizeof(Completion)}; std::uint64_t done{},samples{}; double milliseconds{-1},totalMilliseconds{}; };
using Poll = Result(__cdecl*)(Handle,Completion*); // actual fences; never waits
using Drain = Result(__cdecl*)(Handle); // teardown/reconfigure only, bounded wait
}
