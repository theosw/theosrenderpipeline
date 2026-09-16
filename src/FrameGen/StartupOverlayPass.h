#pragma once
#include "NativeUIPass.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace TheosRenderPipeline
{
    inline bool StartupOverlayEligible(const NativeUIFrame& frame, bool mainOrLoading,
        bool worldCompletedThisFrame, bool nativePassActive)
    {
        return worldCompletedThisFrame && !nativePassActive && !frame.Evaluated() &&
            frame.ReduceStartupViewport(mainOrLoading);
    }
    // One foreground texture, separate from the scene's UI/tagged resources.
    // Allocation never replaces a live texture. Reset requires host retirement.
    class StartupOverlayPass
    {
    public:
        struct Clips
        {
            UINT count{};
            std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> rects{};
        };
        bool Ensure(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& output)
        {
            if (!device || !output.Width || !output.Height || output.SampleDesc.Count != 1) { return false; }
            if (texture_) { return width_ == output.Width && height_ == output.Height && format_ == output.Format; }
            auto desc = output;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = desc.CPUAccessFlags = desc.MiscFlags = 0;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) ||
                FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv)) ||
                FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) { return false; }
            texture_ = std::move(texture); rtv_ = std::move(rtv); srv_ = std::move(srv);
            width_ = desc.Width; height_ = desc.Height; format_ = desc.Format;
            return true;
        }
        bool Begin(ID3D11DeviceContext* context, std::uint64_t frame, bool eligible)
        {
            if (!eligible || !context || !rtv_ || Active()) { return false; }
            savedClips_.count = static_cast<UINT>(savedClips_.rects.size());
            context->RSGetScissorRects(&savedClips_.count, savedClips_.rects.data());
            const NativeUIPass::Target target{rtv_.Get(), nullptr, width_, height_, !Drawn(frame), false};
            if (!bindings_.Enter(context, target)) { return false; }
            context->OMGetBlendState(savedBlend_.ReleaseAndGetAddressOf(), savedBlendFactor_.data(), &savedSampleMask_);
            frame_ = frame; drawn_ = true; thread_ = GetCurrentThreadId();
            context_ = context;
            return true;
        }
        bool End(bool afterRetirement = false)
        {
            if (!Active() || (!afterRetirement && thread_ != GetCurrentThreadId())) { return false; }
            restoring_ = true;
            bindings_.End(context_.Get());
            context_->OMSetBlendState(savedBlend_.Get(), savedBlendFactor_.data(), savedSampleMask_);
            savedBlend_.Reset();
            context_->RSSetScissorRects(savedClips_.count, savedClips_.rects.data());
            restoring_ = false;
            context_.Reset();
            clipContext_ = nullptr; requested_.count = 0; mapped_ = false;
            return true;
        }
        bool Active() const { return bindings_.Active(); }
        bool Internal() const { return bindings_.InternalBind() || restoring_; }
        bool Drawn(std::uint64_t frame) const { return drawn_ && frame_ == frame; }
        void Consume() { drawn_ = false; }
        ID3D11RenderTargetView* RTV() const { return rtv_.Get(); }
        ID3D11ShaderResourceView* SRV() const { return srv_.Get(); }
        ID3D11Texture2D* Texture() const { return texture_.Get(); }

        ID3D11BlendState* ForegroundBlend(ID3D11DeviceContext* context, ID3D11BlendState* original)
        {
            if (!Active() || Internal() || thread_ != GetCurrentThreadId() || !context || !original) { return original; }
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bound;
            context->OMGetRenderTargets(1, bound.GetAddressOf(), nullptr);
            if (bound.Get() != rtv_.Get()) { return original; }
            D3D11_BLEND_DESC desc{}; original->GetDesc(&desc);
            auto& rt = desc.RenderTarget[0];
            // IED 1.7.4: ordinary RGB source-over, but legacy alpha a*(1-a).
            // A transparent foreground needs coverage a + destinationAlpha*(1-a).
            if (desc.AlphaToCoverageEnable || desc.IndependentBlendEnable || !rt.BlendEnable ||
                rt.SrcBlend != D3D11_BLEND_SRC_ALPHA || rt.DestBlend != D3D11_BLEND_INV_SRC_ALPHA ||
                rt.BlendOp != D3D11_BLEND_OP_ADD || rt.SrcBlendAlpha != D3D11_BLEND_INV_SRC_ALPHA ||
                rt.DestBlendAlpha != D3D11_BLEND_ZERO || rt.BlendOpAlpha != D3D11_BLEND_OP_ADD ||
                rt.RenderTargetWriteMask != D3D11_COLOR_WRITE_ENABLE_ALL) { return original; }
            for (const auto& cached : foregroundBlends_) {
                if (cached.original.Get() == original) { return cached.corrected.Get(); }
            }
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            Microsoft::WRL::ComPtr<ID3D11Device> device; context->GetDevice(&device);
            BlendPair pair; pair.original = original;
            if (FAILED(device->CreateBlendState(&desc, &pair.corrected))) { return original; }
            foregroundBlends_.push_back(std::move(pair));
            return foregroundBlends_.back().corrected.Get();
        }

        Clips EndViewport(const void* context)
        {
            Clips original;
            if (context == clipContext_) { original = requested_; }
            requested_.count = 0; clipContext_ = nullptr; mapped_ = false;
            return original;
        }
        bool Viewport(const void* context, UINT count, const D3D11_VIEWPORT* input,
            UINT renderWidth, UINT renderHeight, D3D11_VIEWPORT& output)
        {
            if (!Active() || Internal() || !context || count != 1 || !input ||
                !renderWidth || !renderHeight || input->Width != float(renderWidth) ||
                input->Height != float(renderHeight) || !std::isfinite(input->TopLeftX) ||
                !std::isfinite(input->TopLeftY)) { return false; }
            output = *input; output.Width = float(width_); output.Height = float(height_);
            inputViewport_ = *input; clipContext_ = context; mapped_ = true;
            return true;
        }
        bool Scissors(const void* context, UINT count, const D3D11_RECT* input, D3D11_RECT* output)
        {
            if (!Active() || Internal() || !mapped_ || context != clipContext_ ||
                !count || count > requested_.rects.size() || !input || !output) { return false; }
            requested_.count = count; std::copy_n(input, count, requested_.rects.begin());
            const double sx = double(width_) / inputViewport_.Width, sy = double(height_) / inputViewport_.Height;
            const auto edge = [](double n, bool upper) {
                return static_cast<LONG>(std::clamp(upper ? std::ceil(n) : std::floor(n),
                    double((std::numeric_limits<LONG>::min)()), double((std::numeric_limits<LONG>::max)())));
            };
            for (UINT i = 0; i < count; ++i) {
                const double x = inputViewport_.TopLeftX, y = inputViewport_.TopLeftY;
                output[i] = {edge(x + (input[i].left - x) * sx, false), edge(y + (input[i].top - y) * sy, false),
                    edge(x + (input[i].right - x) * sx, true), edge(y + (input[i].bottom - y) * sy, true)};
                if (input[i].right <= input[i].left) { output[i].right = output[i].left; }
                if (input[i].bottom <= input[i].top) { output[i].bottom = output[i].top; }
            }
            return true;
        }
        bool ResetAfterRetirement()
        {
            if (Active() && !End(true)) { return false; }
            bindings_.ResetEvaluation();
            Consume(); context_.Reset(); srv_.Reset(); rtv_.Reset(); texture_.Reset();
            foregroundBlends_.clear(); savedBlend_.Reset();
            clipContext_ = nullptr; mapped_ = false; requested_.count = 0;
            width_ = height_ = 0; format_ = DXGI_FORMAT_UNKNOWN;
            return true;
        }
    private:
        struct BlendPair { Microsoft::WRL::ComPtr<ID3D11BlendState> original, corrected; };
        std::vector<BlendPair> foregroundBlends_;
        Microsoft::WRL::ComPtr<ID3D11BlendState> savedBlend_;
        std::array<float, 4> savedBlendFactor_{};
        UINT savedSampleMask_{};
        NativeUIPass bindings_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
        UINT width_{}, height_{};
        DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
        std::uint64_t frame_{};
        DWORD thread_{};
        bool drawn_{}, restoring_{}, mapped_{};
        const void* clipContext_{};
        D3D11_VIEWPORT inputViewport_{};
        Clips savedClips_, requested_;
    };
}
