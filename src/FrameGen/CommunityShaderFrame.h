#pragma once

#include "D3D11ContextIsolation.h"
#include "D3D11FrameCopy.h"
#include <cstdint>
#include <limits>

namespace TheosRenderPipeline
{
    // D3D11 snapshots at explicit producer boundaries. Shared D3D12 resources
    // and their fences remain owned by the NVIDIA backend. No CS object layout,
    // private symbol address or shader bytecode identity is needed here.
    class CommunityShaderFrame
    {
    public:
        using Dispatch = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);

        HRESULT CaptureGuides(ID3D11DeviceContext* context, std::uint64_t frame,
            ID3D11Texture2D* motion, ID3D11Texture2D* depth, FrameExtent renderExtent,
            FrameExtent outputExtent)
        {
            if (frame == frame_) { return S_FALSE; }
            frame_ = frame;
            guidesReady_ = sceneReady_ = encodedReady_ = consumed_ = false;
            sceneSource_.Reset(); encodedSource_.Reset(); uiSource_.Reset();
            render_ = renderExtent; output_ = outputExtent;
            if (!output_.width || !output_.height || render_.width > output_.width ||
                render_.height > output_.height || !context || !motion || !depth) { return E_INVALIDARG; }
            D3D11ContextIsolation::Scope scope{isolation_, context};
            if (!scope) { return E_NOINTERFACE; }
            HRESULT result = Ensure(context, motion, render_, DXGI_FORMAT_UNKNOWN,
                D3D11_BIND_SHADER_RESOURCE, motion_);
            if (SUCCEEDED(result)) { result = Ensure(context, depth, render_, DXGI_FORMAT_R32_FLOAT,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, depth_); }
            if (SUCCEEDED(result)) { result = D3D11FrameCopy::Color(context, motion, motion_.Get(), render_); }
            if (SUCCEEDED(result)) { result = depthCopy_.Copy(context, depth, depth_.Get(), render_); }
            guidesReady_ = SUCCEEDED(result);
            return result;
        }

        HRESULT CaptureScene(ID3D11DeviceContext* context, ID3D11Texture2D* scene)
        {
            if (!guidesReady_ || sceneReady_ || consumed_ || !scene) { return E_UNEXPECTED; }
            D3D11ContextIsolation::Scope scope{isolation_, context};
            if (!scope) { return E_NOINTERFACE; }
            auto result = Ensure(context, scene, output_, DXGI_FORMAT_UNKNOWN,
                D3D11_BIND_SHADER_RESOURCE, scene_);
            if (SUCCEEDED(result)) { result = D3D11FrameCopy::Color(context, scene, scene_.Get(), output_); }
            if (FAILED(result)) { return result; }
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            if (!sceneSRV_ || sceneSRVSource_.Get() != scene_.Get()) {
                result = device->CreateShaderResourceView(scene_.Get(), nullptr, sceneSRV_.ReleaseAndGetAddressOf());
                if (FAILED(result)) { return result; }
                sceneSRVSource_ = scene_;
            }
            sceneSource_ = scene;
            sceneReady_ = true;
            return S_OK;
        }

        // Called immediately before a producer Dispatch. Match its actual scene
        // resource and full-size output, replay into our own target with null UI,
        // then restore all changed slots. The caller still performs the original
        // dispatch exactly once. Unrelated passes are left alone.
        HRESULT CaptureDisplayTransform(ID3D11DeviceContext* context,
            UINT x, UINT y, UINT z, Dispatch dispatch)
        {
            if (!sceneReady_ || consumed_ || encodedReady_ || !isolation_.Accepts(context) ||
                !dispatch || !x || !y || z != 1) { return S_FALSE; }
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source, ui;
            ID3D11ShaderResourceView* views[2]{};
            context->CSGetShaderResources(0, 2, views);
            source.Attach(views[0]); ui.Attach(views[1]);
            if (!source || !ui || !References(source.Get(), sceneSource_.Get()) ||
                !References(ui.Get(), uiSource_.Get())) { return S_FALSE; }
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> destination;
            Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
            context->CSGetUnorderedAccessViews(0, 1, &destination);
            context->CSGetShader(&shader, nullptr, nullptr);
            auto output = Texture(destination.Get());
            if (!shader || !output) { return S_FALSE; }
            // Replaying a pass must never write a second producer resource.
            Microsoft::WRL::ComPtr<ID3D11Device> device; context->GetDevice(&device);
            const UINT slots = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
                D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
            ID3D11UnorderedAccessView* others[D3D11_1_UAV_SLOT_COUNT]{};
            context->CSGetUnorderedAccessViews(1, slots - 1, others);
            bool additionalOutput = false;
            for (UINT i = 0; i < slots - 1; ++i) {
                if (others[i]) { additionalOutput = true; others[i]->Release(); }
            }
            if (additionalOutput) { return S_FALSE; }
            D3D11_TEXTURE2D_DESC desc{}; output->GetDesc(&desc);
            if (desc.Width != output_.width || desc.Height != output_.height ||
                !output_.Fits(desc) || References(destination.Get(), sceneSource_.Get())) { return S_FALSE; }
            auto result = Ensure(context, output.Get(), output_, DXGI_FORMAT_UNKNOWN,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, encoded_);
            if (FAILED(result)) { return result; }
            if (!encodedUAV_ || encodedUAVSource_.Get() != encoded_.Get()) {
                result = device->CreateUnorderedAccessView(encoded_.Get(), nullptr, encodedUAV_.ReleaseAndGetAddressOf());
                if (FAILED(result)) { return result; }
                encodedUAVSource_ = encoded_;
            }
            ID3D11ShaderResourceView* cleanViews[]{sceneSRV_.Get(), nullptr};
            auto* cleanOutput = encodedUAV_.Get();
            context->CSSetShaderResources(0, 2, cleanViews);
            context->CSSetUnorderedAccessViews(0, 1, &cleanOutput, nullptr);
            dispatch(context, x, y, z);
            auto* originalOutput = destination.Get();
            context->CSSetUnorderedAccessViews(0, 1, &originalOutput, nullptr);
            context->CSSetShaderResources(0, 2, views);
            encodedSource_ = output;
            return S_OK;
        }

        // The owner calls this only for a copy into its known presentation
        // buffer. A similar-looking intermediate shader is not a display pass.
        bool ConfirmPresentationCopy(ID3D11Resource* source)
        {
            if (!sceneReady_ || consumed_ || encodedReady_ ||
                !D3D11FrameCopy::SameObject(source, encodedSource_.Get())) { return false; }
            encodedReady_ = true;
            return true;
        }

        void SetUIBoundary(ID3D11Texture2D* texture)
        {
            if (sceneReady_ && !consumed_ && !D3D11FrameCopy::SameObject(texture, sceneSource_.Get())) { uiSource_ = texture; }
        }

        ID3D11Texture2D* Hudless(const D3D11_TEXTURE2D_DESC& presentation) const
        {
            if (!sceneReady_ || consumed_) { return nullptr; }
            // A separate UI target implies a later compositor. Even identical
            // formats do not prove its color conversion is a passthrough.
            if (uiSource_ && !encodedReady_) { return nullptr; }
            auto* texture = encodedReady_ ? encoded_.Get() : scene_.Get();
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            return desc.Width == presentation.Width && desc.Height == presentation.Height &&
                desc.Format == presentation.Format ? texture : nullptr;
        }

        bool Ready() const { return guidesReady_ && sceneReady_ && !consumed_; }
        void Consume() { consumed_ = true; }
        ID3D11Texture2D* Motion() const { return guidesReady_ ? motion_.Get() : nullptr; }
        ID3D11Texture2D* Depth() const { return guidesReady_ ? depth_.Get() : nullptr; }
        FrameExtent RenderExtent() const { return render_; }
        FrameExtent OutputExtent() const { return output_; }
        std::uint64_t Frame() const { return frame_; }
        D3D11ContextIsolation& Isolation() { return isolation_; }

        void ResetAfterRetirement()
        {
            isolation_.ResetAfterRetirement(); depthCopy_.ResetViews();
            motion_.Reset(); depth_.Reset(); scene_.Reset(); sceneSource_.Reset(); uiSource_.Reset();
            encoded_.Reset(); encodedSource_.Reset(); encodedUAV_.Reset(); encodedUAVSource_.Reset();
            sceneSRV_.Reset(); sceneSRVSource_.Reset();
            guidesReady_ = sceneReady_ = encodedReady_ = consumed_ = false;
            frame_ = (std::numeric_limits<std::uint64_t>::max)(); render_ = output_ = {};
        }

        static Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture(ID3D11View* view)
        {
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (view) { view->GetResource(&resource); }
            if (resource) { resource.As(&texture); }
            return texture;
        }

    private:
        static bool References(ID3D11View* view, ID3D11Texture2D* texture)
        {
            return D3D11FrameCopy::SameObject(Texture(view).Get(), texture);
        }

        static HRESULT Ensure(ID3D11DeviceContext* context, ID3D11Texture2D* source,
            FrameExtent extent, DXGI_FORMAT format, UINT bindings,
            Microsoft::WRL::ComPtr<ID3D11Texture2D>& target)
        {
            D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
            if (!extent.Fits(desc)) { return E_INVALIDARG; }
            Microsoft::WRL::ComPtr<ID3D11Device> device, sourceDevice;
            context->GetDevice(&device); source->GetDevice(&sourceDevice);
            if (!D3D11FrameCopy::SameObject(device.Get(), sourceDevice.Get())) { return E_INVALIDARG; }
            desc.Width = extent.width; desc.Height = extent.height;
            if (format != DXGI_FORMAT_UNKNOWN) { desc.Format = format; }
            desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bindings;
            desc.CPUAccessFlags = desc.MiscFlags = 0;
            if (target) {
                D3D11_TEXTURE2D_DESC old{}; target->GetDesc(&old);
                if (old.Width == desc.Width && old.Height == desc.Height && old.Format == desc.Format) { return S_OK; }
            }
            Microsoft::WRL::ComPtr<ID3D11Texture2D> next;
            const auto result = device->CreateTexture2D(&desc, nullptr, &next);
            if (SUCCEEDED(result)) { target = std::move(next); }
            return result;
        }

        D3D11ContextIsolation isolation_;
        D3D11FrameCopy::Depth depthCopy_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> motion_, depth_, scene_, sceneSource_, uiSource_, encoded_, encodedSource_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneSRV_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sceneSRVSource_, encodedUAVSource_;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> encodedUAV_;
        FrameExtent render_{}, output_{};
        std::uint64_t frame_{(std::numeric_limits<std::uint64_t>::max)()};
        bool guidesReady_{}, sceneReady_{}, encodedReady_{}, consumed_{};
    };
}
