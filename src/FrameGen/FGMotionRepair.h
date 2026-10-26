#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>

namespace TheosRenderPipeline
{
    // Underwater, Skyrim leaves its motion-vector target holding one value for
    // the whole frame instead of real motion, and frame generation warps the
    // picture by it (a faint offset copy, a ghost while moving). Each frame a
    // grid of samples compares the motion vectors with the motion the camera
    // alone implies at that depth; when nearly all disagree, the frame's motion
    // is replaced by that camera motion. Otherwise it is copied unchanged.
    // Everything stays on the GPU: the decision never waits for a readback.
    // It runs only while the game reports the player underwater: above water
    // (dialogue close-ups, for one) the camera check alone could flip between
    // engine and camera motion and make NR flicker, so motion passes through.
    class FGMotionRepair
    {
    public:
        static FGMotionRepair& Get() { static FGMotionRepair repair; return repair; }
        void Enable(bool a_enabled) { enabled_.store(a_enabled, std::memory_order_relaxed); }
        bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }
        // Returns the motion to hand to frame generation: the repaired copy, or
        // `motion` itself when disabled, above water or unavailable.
        // clipToPrevClip is row-major for row vectors, as Streamline receives it.
        ID3D11Texture2D* Apply(ID3D11DeviceContext* context, ID3D11Texture2D* motion, ID3D11Texture2D* depth,
            const float (&clipToPrevClip)[16], bool depthInverted, std::uint32_t renderWidth, std::uint32_t renderHeight,
            bool underwater);
        // Diagnostics read back a few frames late, without waiting: of the 64x36
        // samples, how many disagreed with the camera and how many differed from
        // the frame's centre motion (a stuck frame has almost none).
        struct Decision
        {
            std::uint64_t frame{};
            std::uint32_t disagree{}, varied{};
            bool repaired{};
        };
        static constexpr std::uint32_t kSamples = 64 * 36;
        // Returns true once per newly read frame.
        bool TakeDecision(Decision& out);
    private:
        bool Initialize(ID3D11DeviceContext* context);
        bool EnsureOutput(ID3D11Texture2D* motion);
        std::atomic_bool enabled_{ true };
        bool failed_{ false };
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> count_, write_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants_, counter_;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> counterUAV_, outputUAV_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> output_, motion_, depth_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motionSRV_, depthSRV_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> readback_[3];
        std::uint64_t readbackFrame_[3]{}, applied_{};
        Decision latest_{};
        bool fresh_{};
    };
}
