#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <string_view>

namespace TheosRenderPipeline
{
    // Native companions for the game's two-target UI draws and depth reads.
    // Existing resources are never replaced here: changed contracts require
    // the host's retirement path to call ResetAfterRetirement first.
    class NativeUIAttachments
    {
    public:
        static bool References(ID3D11View* view, ID3D11Resource* expected)
        {
            if (!view || !expected) { return false; }
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            return resource.Get() == expected;
        }

        bool ObserveENBBoundary(ID3D11View* view)
        {
            if (!view) { return false; }
            if (enbUI_) { return References(view, enbUI_.Get()); }
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            std::array<char, 128> name{};
            UINT length = static_cast<UINT>(name.size());
            if (resource && SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName, &length, name.data())) && length <= name.size()) {
                std::string_view actual(name.data(), length);
                if (!actual.empty() && actual.back() == '\0') { actual.remove_suffix(1); }
                if (actual == "HDR::UiTexture") { enbUI_ = resource; }
            }
            // The reference discovers the resource on this call and uses its
            // separate no-evaluation branch only on subsequent identity matches.
            return false;
        }
        bool IsENBTarget(ID3D11View* view) const { return References(view, enbUI_.Get()); }

        HRESULT Ensure(ID3D11Device* device, ID3D11DeviceContext* context,
            ID3D11Texture2D* motion, ID3D11Texture2D* depth, UINT width, UINT height)
        {
            if (!device || !context || !motion || !depth || !width || !height) { return E_INVALIDARG; }
            D3D11_TEXTURE2D_DESC motionDesc{}, depthDesc{};
            motion->GetDesc(&motionDesc); depth->GetDesc(&depthDesc);
            // The source host captures this depth family and single-sample RG16 motion.
            if (motionDesc.SampleDesc.Count != 1 || depthDesc.SampleDesc.Count != 1 ||
                motionDesc.Format != DXGI_FORMAT_R16G16_FLOAT ||
                (depthDesc.Format != DXGI_FORMAT_R24G8_TYPELESS && depthDesc.Format != DXGI_FORMAT_D24_UNORM_S8_UINT)) {
                return E_INVALIDARG;
            }
            if (auxiliary_ || depthSubstitute_) {
                return width == width_ && height == height_ && auxiliaryRTV_ && depthSRV_ ? S_OK : E_UNEXPECTED;
            }
            motionDesc.Width = width; motionDesc.Height = height;
            motionDesc.MipLevels = motionDesc.ArraySize = 1;
            motionDesc.Usage = D3D11_USAGE_DEFAULT;
            motionDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            motionDesc.CPUAccessFlags = motionDesc.MiscFlags = 0;
            depthDesc = motionDesc;
            depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
            depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> auxiliary, substitute;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            auto hr = device->CreateTexture2D(&motionDesc, nullptr, &auxiliary);
            if (SUCCEEDED(hr)) { hr = device->CreateRenderTargetView(auxiliary.Get(), nullptr, &rtv); }
            if (SUCCEEDED(hr)) { hr = device->CreateTexture2D(&depthDesc, nullptr, &substitute); }
            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            if (SUCCEEDED(hr)) { hr = device->CreateDepthStencilView(substitute.Get(), &dsvDesc, &dsv); }
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            if (SUCCEEDED(hr)) { hr = device->CreateShaderResourceView(substitute.Get(), &srvDesc, &srv); }
            if (FAILED(hr)) { return hr; }
            auxiliary_ = std::move(auxiliary); auxiliaryRTV_ = std::move(rtv);
            depthSubstitute_ = std::move(substitute); depthSRV_ = std::move(srv);
            width_ = width; height_ = height;
            context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            ClearAuxiliary(context);
            return S_OK;
        }

        void ClearAuxiliary(ID3D11DeviceContext* context) const
        {
            if (context && auxiliaryRTV_) { const float zero[4]{}; context->ClearRenderTargetView(auxiliaryRTV_.Get(), zero); }
        }
        UINT SubstituteDepths(UINT count, ID3D11ShaderResourceView* const* views,
            ID3D11ShaderResourceView** outputs, ID3D11Resource* capturedGuide,
            ID3D11Resource* backgroundDepth = nullptr) const
        {
            if (!views || !outputs || count > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) { return 0; }
            UINT replaced = 0;
            for (UINT i = 0; i < count; ++i) {
                outputs[i] = views[i];
                // Match actual roles, not dimensions or a shader slot number.
                if (depthSRV_ && (References(views[i], backgroundDepth) || References(views[i], capturedGuide))) {
                    outputs[i] = depthSRV_.Get(); ++replaced;
                }
            }
            return replaced;
        }
        bool RedirectTargets(UINT count, ID3D11RenderTargetView* const* inputs,
            ID3D11RenderTargetView* color, ID3D11RenderTargetView** outputs) const
        {
            if (!count || count > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT || !inputs || !color || !auxiliaryRTV_) { return false; }
            for (UINT i = 2; i < count; ++i) { if (inputs[i]) { return false; } }
            for (UINT i = 0; i < count; ++i) { outputs[i] = inputs[i]; }
            outputs[0] = color;
            if (count > 1) { outputs[1] = auxiliaryRTV_.Get(); }
            return true;
        }
        ID3D11RenderTargetView* AuxiliaryRTV() const { return auxiliaryRTV_.Get(); }
        ID3D11ShaderResourceView* DepthSRV() const { return depthSRV_.Get(); }
        void ResetAfterRetirement()
        {
            auxiliaryRTV_.Reset(); auxiliary_.Reset(); depthSRV_.Reset(); depthSubstitute_.Reset(); enbUI_.Reset();
            width_ = height_ = 0;
        }
    private:
        Microsoft::WRL::ComPtr<ID3D11Texture2D> auxiliary_, depthSubstitute_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> auxiliaryRTV_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSRV_;
        Microsoft::WRL::ComPtr<ID3D11Resource> enbUI_;
        UINT width_{}, height_{};
    };

    struct NativeUIDepthInputs
    {
        const NativeUIAttachments* attachments{};
        ID3D11Resource* capturedGuide{};
        ID3D11Resource* backgroundDepth{};
    };

    // Draw-time coverage for cached or grouped shader-resource bindings. The
    // caller keeps host remapping disabled through this scope's restoration.
    class NativeUIDepthScope
    {
    public:
        NativeUIDepthScope(ID3D11DeviceContext* context, const NativeUIDepthInputs& inputs) : context_(context)
        {
            if (!context || !inputs.attachments || !inputs.attachments->DepthSRV()) { return; }
            ID3D11ShaderResourceView* raw[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
            ID3D11ShaderResourceView* replacements[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
            context->PSGetShaderResources(0, static_cast<UINT>(originals_.size()), raw);
            for (UINT i = 0; i < originals_.size(); ++i) { originals_[i].Attach(raw[i]); }
            count_ = inputs.attachments->SubstituteDepths(static_cast<UINT>(originals_.size()), raw,
                replacements, inputs.capturedGuide, inputs.backgroundDepth);
            for (UINT i = 0; i < originals_.size(); ++i) {
                changed_[i] = raw[i] != replacements[i];
                if (changed_[i]) { context->PSSetShaderResources(i, 1, &replacements[i]); }
            }
        }
        NativeUIDepthScope(const NativeUIDepthScope&) = delete;
        NativeUIDepthScope& operator=(const NativeUIDepthScope&) = delete;
        ~NativeUIDepthScope()
        {
            for (UINT i = 0; i < originals_.size(); ++i) {
                if (changed_[i]) { auto* view = originals_[i].Get(); context_->PSSetShaderResources(i, 1, &view); }
            }
        }
        UINT Count() const { return count_; }
    private:
        ID3D11DeviceContext* context_{};
        std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> originals_;
        std::array<bool, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> changed_{};
        UINT count_{};
    };
}
