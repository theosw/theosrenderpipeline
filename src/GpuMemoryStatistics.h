#pragma once

// Dedicated video memory from D3DKMTQueryStatistics, the kernel statistics behind
// Task Manager's GPU memory figures. DXGI QueryVideoMemoryInfo reports only what
// the process's budget accounting charges, which can be gigabytes below the
// process's actual dedicated allocations, so it cannot tell how full the card is.
// Header-only so every target that builds NR admission gets it without new sources.

#include <Windows.h>
#include <bcrypt.h> // NTSTATUS for d3dkmthk.h
#include <d3dkmthk.h>

#include <cstdint>
#include <optional>

namespace TheosRenderPipeline::GpuMemory
{
    struct Statistics
    {
        std::uint64_t adapterLimit{};     // capacity of the adapter's dedicated (non-aperture) segments
        std::uint64_t adapterResident{};  // bytes resident there, all processes, as in Task Manager
        std::uint64_t processCommitted{}; // this process's bytes in those segments
        std::uint64_t processDemoted{};   // this process's local-group bytes moved out under budget pressure
        bool processKnown{};              // false before the process has a device on the adapter

        std::uint64_t AdapterFree() const { return adapterResident < adapterLimit ? adapterLimit - adapterResident : 0; }
    };

    // Cheap kernel calls (no I/O); safe on the render thread. Empty when the
    // function or adapter is unavailable or the adapter has no dedicated segment.
    inline std::optional<Statistics> Query(const LUID& adapter)
    {
        using QueryStatistics = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);
        static const auto query = [] {
            const auto gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return gdi ? reinterpret_cast<QueryStatistics>(GetProcAddress(gdi, "D3DKMTQueryStatistics")) : nullptr;
        }();
        if (!query) { return std::nullopt; }
        const auto run = [&](D3DKMT_QUERYSTATISTICS_TYPE type, auto&& prepare) -> std::optional<D3DKMT_QUERYSTATISTICS> {
            D3DKMT_QUERYSTATISTICS request{};
            request.Type = type;
            request.AdapterLuid = adapter;
            prepare(request);
            if (query(&request) < 0) { return std::nullopt; }
            return request;
        };
        const auto adapterInfo = run(D3DKMT_QUERYSTATISTICS_ADAPTER, [](auto&) {});
        if (!adapterInfo) { return std::nullopt; }
        Statistics result;
        result.processKnown = true;
        bool dedicated{};
        for (ULONG id = 0; id < adapterInfo->QueryResult.AdapterInformation.NbSegments; ++id) {
            const auto segment = run(D3DKMT_QUERYSTATISTICS_SEGMENT, [&](auto& request) { request.QuerySegment.SegmentId = id; });
            if (!segment) { return std::nullopt; }
            const auto& info = segment->QueryResult.SegmentInformation;
            if (info.Aperture) { continue; }
            dedicated = true;
            result.adapterLimit += info.CommitLimit;
            result.adapterResident += info.BytesResident;
            const auto process = run(D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT, [&](auto& request) {
                request.hProcess = GetCurrentProcess();
                request.QueryProcessSegment.SegmentId = id;
            });
            if (process) { result.processCommitted += process->QueryResult.ProcessSegmentInformation.BytesCommitted; }
            else { result.processKnown = false; }
        }
        if (!dedicated || !result.adapterLimit) { return std::nullopt; }
        const auto group = result.processKnown ? run(D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP, [](auto& request) {
            request.hProcess = GetCurrentProcess();
            request.QueryProcessSegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
        }) : std::nullopt;
        if (group) {
            for (const auto bytes : group->QueryResult.ProcessSegmentGroupInformation.Demoted) { result.processDemoted += bytes; }
        }
        if (!result.processKnown) { result.processCommitted = result.processDemoted = 0; }
        return result;
    }
}
