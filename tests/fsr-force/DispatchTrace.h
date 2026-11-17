#pragma once
#include <d3d12.h>
#include <filesystem>
namespace DispatchTrace
{
    // Fixture process only. Single recording thread and one submitted list.
    // No feature or adapter-query interception. Installed before SDK initialization.
    void Install(ID3D12Device*, ID3D12GraphicsCommandList*, ID3D12CommandQueue*, const std::filesystem::path&);
    void Begin(unsigned context, unsigned frame);
    void End();
    void Complete(UINT64 fence);
    bool Available();
    void Finish();
}
