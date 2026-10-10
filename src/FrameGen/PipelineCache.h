#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace TheosRenderPipeline
{
    // One D3D12 pipeline library per Intel runtime and session, optionally
    // persisted. Intel's SDKs accept it at initialization for pipeline caching,
    // so a recreated XeFG presenter can load pipelines an earlier one stored
    // instead of compiling them on its first generated frames. Every failure
    // leaves the SDK compiling as before, without a library.
    class PipelineCache final
    {
    public:
        PipelineCache() = default;
        PipelineCache(const PipelineCache&) = delete;
        PipelineCache& operator=(const PipelineCache&) = delete;
        // An empty file keeps the library in memory only. A stale, foreign or
        // damaged file is ignored and replaced by the next save.
        void Open(ID3D12Device* device, const std::filesystem::path& file);
        bool Opened() const { return opened_; }
        ID3D12PipelineLibrary* Library() const { return library_.Get(); }
        // Writes the file when the library holds more than it last read or wrote.
        void SaveIfGrown();
        const std::string& Status() const { return status_; }
    private:
        std::vector<std::uint8_t> blob_; // D3D12 reads from it for the library's lifetime.
        Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> library_;
        std::filesystem::path file_;
        std::string status_{"not opened"};
        SIZE_T savedSize_{};
        bool opened_{};
    };

    // Identifies a loaded runtime build: PE timestamp and image size.
    std::string ModuleFingerprint(HMODULE module);
    // %LOCALAPPDATA%/TheosRenderPipeline/PipelineCache/<name>-<GPU>-<driver>-<runtime>.bin,
    // or empty when the folder is unavailable. Outside MO2's virtual Data tree.
    std::filesystem::path PipelineCacheFile(const char* name, UINT vendor, UINT device, LARGE_INTEGER driver, HMODULE runtime);
}
