#pragma once
#include <dxgi.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace TheosRenderPipeline
{
    // Diagnostic references only. Drop buffers before the chain so their counts
    // do not include the probe's own chain owner. Never use Release counts as a
    // retirement/admission decision: COM implementations may report estimates.
    // A reuse leaves one explicit TRP chain owner, not a portable raw count of
    // one. Buffer/chain counts may include DXGI/driver internals. Compare repeated
    // lifetimes and a final full release instead of inferring leaked owners from
    // a single nonzero number.
    class PresenterReleaseProbe
    {
    public:
        explicit PresenterReleaseProbe(IDXGISwapChain* chain) : chain_(chain)
        {
            if (!chain) { return; }
            described = chain->GetDesc(&desc);
            if (FAILED(described)) { return; }
            BOOL fullscreen{};
            fullscreenResult = chain->GetFullscreenState(&fullscreen, nullptr);
            windowed = SUCCEEDED(fullscreenResult) ? !fullscreen : desc.Windowed;
            buffers_.resize(desc.BufferCount);
            for (UINT i = 0; i < desc.BufferCount; ++i) {
                auto& entry = buffers_[i];
                entry.result = chain->GetBuffer(i, IID_PPV_ARGS(&entry.buffer));
                entry.identity = entry.buffer.Get();
            }
        }
        template<class LogBuffer, class LogChain>
        void Release(LogBuffer&& bufferLog, LogChain&& chainLog)
        {
            for (UINT i = 0; i < buffers_.size(); ++i) {
                auto& entry = buffers_[i];
                const auto remaining = entry.buffer ? entry.buffer.Detach()->Release() : 0;
                bufferLog(i, entry.identity, entry.result, remaining);
            }
            const auto identity = chain_.Get();
            const auto remaining = chain_ ? chain_.Detach()->Release() : 0;
            chainLog(identity, remaining);
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        HRESULT described{E_POINTER}, fullscreenResult{E_POINTER};
        bool windowed{};
    private:
        struct Buffer {
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
            ID3D12Resource* identity{};
            HRESULT result{E_FAIL};
        };
        Microsoft::WRL::ComPtr<IDXGISwapChain> chain_;
        std::vector<Buffer> buffers_;
    };
}
