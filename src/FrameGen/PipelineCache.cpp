#include "PipelineCache.h"
#include <ShlObj.h>
#include <format>
#include <fstream>

namespace TheosRenderPipeline
{
    void PipelineCache::Open(ID3D12Device* device, const std::filesystem::path& file)
    {
        if (opened_ || !device) { return; }
        opened_ = true;
        status_.clear();
        Microsoft::WRL::ComPtr<ID3D12Device1> device1;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) { status_ = "pipeline libraries unsupported by this device"; return; }
        file_ = file;
        std::error_code error;
        if (!file_.empty() && std::filesystem::is_regular_file(file_, error)) {
            std::ifstream in(file_, std::ios::binary);
            blob_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        HRESULT hr = E_FAIL;
        if (!blob_.empty()) {
            hr = device1->CreatePipelineLibrary(blob_.data(), blob_.size(), IID_PPV_ARGS(&library_));
            if (SUCCEEDED(hr)) {
                savedSize_ = blob_.size();
                status_ = std::format("loaded {} KiB", blob_.size() / 1024);
                return;
            }
            // Driver update, other GPU or a damaged file: start empty and replace it.
            status_ = std::format("ignored saved file (0x{:08X}); ", static_cast<unsigned>(hr));
            blob_.clear();
        }
        hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library_));
        if (FAILED(hr)) { library_.Reset(); status_ += std::format("library unavailable (0x{:08X})", static_cast<unsigned>(hr)); return; }
        status_ += "started empty";
    }

    void PipelineCache::SaveIfGrown()
    {
        if (!library_ || file_.empty()) { return; }
        const auto size = library_->GetSerializedSize();
        if (size <= savedSize_) { return; }
        std::vector<std::uint8_t> data(size);
        if (FAILED(library_->Serialize(data.data(), data.size()))) { status_ = "serialize failed"; savedSize_ = size; return; }
        std::error_code error;
        std::filesystem::create_directories(file_.parent_path(), error);
        auto temporary = file_; temporary += L".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!out) { status_ = "write failed"; savedSize_ = size; return; }
        }
        if (!MoveFileExW(temporary.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            status_ = std::format("replace failed ({})", GetLastError());
            std::filesystem::remove(temporary, error);
            savedSize_ = size; // Do not retry every check after a persistent failure.
            return;
        }
        savedSize_ = size;
        status_ = std::format("saved {} KiB", size / 1024);
    }

    std::string ModuleFingerprint(HMODULE module)
    {
        if (!module) { return "none"; }
        const auto* base = reinterpret_cast<const std::uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return "unknown"; }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) { return "unknown"; }
        return std::format("{:08x}{:08x}", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    }

    std::filesystem::path PipelineCacheFile(const char* name, UINT vendor, UINT device, LARGE_INTEGER driver, HMODULE runtime)
    {
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) {
            CoTaskMemFree(local);
            return {};
        }
        std::filesystem::path root(local);
        CoTaskMemFree(local);
        const auto d = static_cast<std::uint64_t>(driver.QuadPart);
        return root / L"TheosRenderPipeline" / L"PipelineCache" /
            std::format("{}-{:04x}{:04x}-{}.{}.{}.{}-{}.bin", name, vendor, device,
                d >> 48, (d >> 32) & 0xffff, (d >> 16) & 0xffff, d & 0xffff, ModuleFingerprint(runtime));
    }
}
