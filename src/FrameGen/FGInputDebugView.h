#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>

namespace TheosRenderPipeline
{
    // Lab diagnostic, never saved: a strip of three panels built from the exact
    // motion vectors, depth and camera matrices handed to frame generation.
    //   1. motion vectors (grey = none; red/green = x/y)
    //   2. depth (dark = near, light = far)
    //   3. disagreement between the motion vectors and the motion the camera
    //      alone implies at that depth (black = agree, red = 8+ pixels off)
    // Static scenery should stay black in panel 3 while the camera moves.
    class FGInputDebugView
    {
    public:
        static FGInputDebugView& Get() { static FGInputDebugView view; return view; }
        void Enable(bool a_enabled) { enabled_.store(a_enabled, std::memory_order_relaxed); }
        bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }
        // Render thread, at the frame-generation input boundary. clipToPrevClip
        // is row-major for row vectors, as Streamline receives it.
        void Record(ID3D11DeviceContext* context, ID3D11Texture2D* motion, ID3D11Texture2D* depth,
            const float (&clipToPrevClip)[16], bool depthInverted, std::uint32_t renderWidth, std::uint32_t renderHeight);
        // Overlay thread reads the last strip; null until the first Record.
        ID3D11ShaderResourceView* Strip() const { return stripSRV_.Get(); }
        std::uint32_t PanelWidth() const { return panelWidth_; }
        std::uint32_t PanelHeight() const { return panelHeight_; }
        const char* Status() const { return status_; }
    private:
        bool Initialize(ID3D11DeviceContext* context);
        ID3D11ShaderResourceView* View(ID3D11Texture2D* texture, Microsoft::WRL::ComPtr<ID3D11Texture2D>& cached,
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view);
        std::atomic_bool enabled_{ false };
        bool failed_{ false };
        const char* status_{ "waiting for a world frame" };
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> strip_, motion_, depth_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> stripSRV_, motionSRV_, depthSRV_;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> stripUAV_;
        std::uint32_t panelWidth_{}, panelHeight_{};
    };
}
