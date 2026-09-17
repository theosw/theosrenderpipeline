#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

namespace TheosRenderPipeline
{
    // Borrow the immediate context with an empty pipeline, then restore every
    // producer binding. The scratch state is reused between frame boundaries.
    class D3D11ContextIsolation
    {
    public:
        bool Accepts(ID3D11DeviceContext* context) const
        {
            return context && context == originalContext_ && !active_ &&
                context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
        }

        bool Begin(ID3D11DeviceContext* context)
        {
            if (!context || active_ || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return false; }
            if (!context_) {
                Microsoft::WRL::ComPtr<ID3D11Device> device;
                Microsoft::WRL::ComPtr<ID3D11Device1> device1;
                context->GetDevice(&device);
                if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context_))) ||
                    FAILED(device.As(&device1))) { context_.Reset(); return false; }
                const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
                D3D_FEATURE_LEVEL selected{};
                if (FAILED(device1->CreateDeviceContextState(0, levels, 2, D3D11_SDK_VERSION,
                    __uuidof(ID3D11Device), &selected, &scratch_))) { context_.Reset(); return false; }
                originalContext_ = context;
            } else if (context != originalContext_) { return false; }
            context_->SwapDeviceContextState(scratch_.Get(), &saved_);
            active_ = true;
            context_->ClearState();
            return true;
        }

        void End()
        {
            if (!active_) { return; }
            context_->ClearState();
            context_->SwapDeviceContextState(saved_.Get(), nullptr);
            saved_.Reset();
            active_ = false;
        }

        void ResetAfterRetirement()
        {
            End(); scratch_.Reset(); context_.Reset(); originalContext_ = nullptr;
        }

        class Scope
        {
        public:
            Scope(D3D11ContextIsolation& owner, ID3D11DeviceContext* context) :
                owner_(owner), valid_(owner.Begin(context)) {}
            ~Scope() { if (valid_) { owner_.End(); } }
            explicit operator bool() const { return valid_; }
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;
        private:
            D3D11ContextIsolation& owner_;
            bool valid_{};
        };

    private:
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
        Microsoft::WRL::ComPtr<ID3DDeviceContextState> scratch_, saved_;
        ID3D11DeviceContext* originalContext_{};
        bool active_{};
    };
}
