#pragma once
#include <d3d11.h>
#include <algorithm>
#include <array>

// Saves the compute bindings a TRP pass touches on the game's context and puts
// them back afterwards. Use this instead of SwapDeviceContextState: a state swap
// rewrites the immediate context's dispatch table, which drops the native UI
// routing hooks installed in it (HUD routing, audit HOOKS-2).
namespace TheosRenderPipeline
{
    class D3D11ComputeBindings
    {
    public:
        static constexpr UINT kSlots = 4;

        D3D11ComputeBindings(ID3D11DeviceContext* context, UINT constantBuffers, UINT resources, UINT unorderedViews) :
            context_(context), constantBuffers_((std::min)(constantBuffers, kSlots)), resources_((std::min)(resources, kSlots)),
            unorderedViews_((std::min)(unorderedViews, kSlots))
        {
            if (!context_) { return; }
            classCount_ = static_cast<UINT>(classes_.size());
            context_->CSGetShader(&shader_, classes_.data(), &classCount_);
            if (constantBuffers_) { context_->CSGetConstantBuffers(0, constantBuffers_, buffers_.data()); }
            if (resources_) { context_->CSGetShaderResources(0, resources_, views_.data()); }
            if (unorderedViews_) { context_->CSGetUnorderedAccessViews(0, unorderedViews_, uavs_.data()); }
        }

        ~D3D11ComputeBindings()
        {
            if (!context_) { return; }
            // Unbind the pass's outputs first so a restored input is never also bound for writing.
            std::array<ID3D11UnorderedAccessView*, kSlots> noUAVs{};
            if (unorderedViews_) { context_->CSSetUnorderedAccessViews(0, unorderedViews_, noUAVs.data(), nullptr); }
            if (resources_) { context_->CSSetShaderResources(0, resources_, views_.data()); }
            if (constantBuffers_) { context_->CSSetConstantBuffers(0, constantBuffers_, buffers_.data()); }
            if (unorderedViews_) {
                // -1 keeps each append/consume counter as it is.
                std::array<UINT, kSlots> keep; keep.fill(~0u);
                context_->CSSetUnorderedAccessViews(0, unorderedViews_, uavs_.data(), keep.data());
            }
            context_->CSSetShader(shader_, classes_.data(), classCount_);
            for (auto* b : buffers_) { if (b) { b->Release(); } }
            for (auto* v : views_) { if (v) { v->Release(); } }
            for (auto* u : uavs_) { if (u) { u->Release(); } }
            for (UINT i = 0; i < classCount_; ++i) { if (classes_[i]) { classes_[i]->Release(); } }
            if (shader_) { shader_->Release(); }
        }

        D3D11ComputeBindings(const D3D11ComputeBindings&) = delete;
        D3D11ComputeBindings& operator=(const D3D11ComputeBindings&) = delete;

    private:
        ID3D11DeviceContext* context_{};
        UINT constantBuffers_{}, resources_{}, unorderedViews_{};
        ID3D11ComputeShader* shader_{};
        std::array<ID3D11ClassInstance*, 256> classes_{};
        UINT classCount_{};
        std::array<ID3D11Buffer*, kSlots> buffers_{};
        std::array<ID3D11ShaderResourceView*, kSlots> views_{};
        std::array<ID3D11UnorderedAccessView*, kSlots> uavs_{};
    };
}
