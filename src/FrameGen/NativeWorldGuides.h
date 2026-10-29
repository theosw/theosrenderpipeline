#pragma once

#include "D3D11FrameCopy.h"
#include "NativeUIFrame.h"
#include <atomic>
#include <cstdint>
#include <utility>

namespace TheosRenderPipeline
{
    enum class NativeDepthRole { None, PostZPrepass, Main };

    inline const char* NativeDepthRoleName(NativeDepthRole role)
    {
        switch (role) {
        case NativeDepthRole::PostZPrepass: return "post-z-prepass";
        case NativeDepthRole::Main: return "main";
        default: return "unavailable";
        }
    }

    struct NativeGuideSnapshot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> motion, depth;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        NativeDepthRole depthRole{NativeDepthRole::None};
        FrameExtent extent{};
        std::uint64_t present{}, gameFrame{}, generation{};
    };

    // Only the completed native world producer supplies these slots. Allocation
    // descriptors validate known roles; CreateTexture2D never supplies candidates.
    struct NativeGuideSources
    {
        ID3D11Texture2D* motion{};       // kMOTION_VECTOR
        ID3D11Texture2D* postZDepth{};   // kPOST_ZPREPASS_COPY
        ID3D11Texture2D* mainDepth{};    // explicit kMAIN fallback
    };

    class NativeWorldGuides
    {
    public:
        enum class Observation { Unavailable, Unchanged, Changed, RetirementFailed, WrongThread };

        // The latest world boundary's thread owns the snapshot. Readers on other
        // threads fail closed without touching the non-atomic owning snapshot.
        const NativeGuideSnapshot* Current(std::uint64_t present) const
        {
            if (thread_.load(std::memory_order_acquire) != GetCurrentThreadId()) { return nullptr; }
            return valid_ && snapshot_.present == present ? &snapshot_ : nullptr;
        }

        template<class Retire, class RequestReset>
        Observation Observe(ID3D11Device* device, NativeGuideSources sources, FrameExtent extent,
            std::uint64_t present, std::uint64_t gameFrame, bool worldEligible,
            Retire&& retire, RequestReset&& requestReset)
        {
            const auto thread = GetCurrentThreadId();
            const auto owner = thread_.load(std::memory_order_acquire);
            if (owner != thread) {
                // A non-world callback cannot claim or take over ownership.
                if (!worldEligible) { return owner ? Observation::WrongThread : Observation::Unavailable; }
                // Ownership follows the thread drawing the world: LoreRim draws
                // its loading screen on a worker and gameplay on the main thread.
                // The engine draws on one thread at a time, so the old owner has
                // stopped; its readers fail closed from here on. The pair itself
                // is unchanged, so this alone does not reset history.
                thread_.store(thread, std::memory_order_release);
                observed_ = valid_ = false;
            }
            // Keep all consumers on one pair even if the engine repeats a boundary.
            if (observed_ && observedPresent_ == present) {
                return valid_ ? Observation::Unchanged : Observation::Unavailable;
            }
            observed_ = true; observedPresent_ = present;
            valid_ = false;
            if (!worldEligible || !Valid(device, sources.motion, extent, true)) { return Observation::Unavailable; }

            NativeGuideSnapshot candidate;
            candidate.motion = sources.motion;
            if (Valid(device, sources.postZDepth, extent, false)) {
                candidate.depth = sources.postZDepth; candidate.depthRole = NativeDepthRole::PostZPrepass;
            } else if (Valid(device, sources.mainDepth, extent, false)) {
                candidate.depth = sources.mainDepth; candidate.depthRole = NativeDepthRole::Main;
            } else { return Observation::Unavailable; }
            candidate.device = device; candidate.extent = extent;
            candidate.present = present; candidate.gameFrame = gameFrame;

            const bool sameResources = D3D11FrameCopy::SameObject(snapshot_.motion.Get(), candidate.motion.Get()) &&
                D3D11FrameCopy::SameObject(snapshot_.depth.Get(), candidate.depth.Get()) &&
                D3D11FrameCopy::SameObject(snapshot_.device.Get(), candidate.device.Get());
            // A skipped producer frame with the same resources is not a new
            // generation: the next frame's camera sees the game-frame gap and
            // resets the upscaler, NR and frame generation itself.
            const bool changed = !sameResources || snapshot_.depthRole != candidate.depthRole ||
                snapshot_.extent.width != extent.width || snapshot_.extent.height != extent.height;
            // These engine inputs are read on D3D11; downstream NR/FG uses its
            // owned copies. Retire queued native reads before dropping our last
            // old-pair references. Failure keeps ownership but removes readiness.
            if (!sameResources && snapshot_.motion && !retire()) { return Observation::RetirementFailed; }
            candidate.generation = snapshot_.generation + (changed ? 1 : 0);
            snapshot_ = std::move(candidate);
            valid_ = true;
            if (changed) { requestReset(); }
            return changed ? Observation::Changed : Observation::Unchanged;
        }

        // Host lifecycle has already retired GPU work and serialized all readers.
        void ResetAfterRetirement()
        {
            const auto generation = snapshot_.generation;
            snapshot_ = {}; snapshot_.generation = generation;
            valid_ = observed_ = false;
            thread_.store(0, std::memory_order_release);
        }

    private:
        static bool Valid(ID3D11Device* device, ID3D11Texture2D* texture, FrameExtent extent, bool motion)
        {
            if (!device || !texture) { return false; }
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            if (!extent.Fits(desc) || desc.Width != extent.width || desc.Height != extent.height) { return false; }
            const UINT bindings = motion ? D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET : D3D11_BIND_DEPTH_STENCIL;
            if ((desc.BindFlags & bindings) != bindings ||
                (motion ? desc.Format != DXGI_FORMAT_R16G16_FLOAT :
                    desc.Format != DXGI_FORMAT_R24G8_TYPELESS && desc.Format != DXGI_FORMAT_D24_UNORM_S8_UINT)) { return false; }
            Microsoft::WRL::ComPtr<ID3D11Device> producerDevice;
            texture->GetDevice(&producerDevice);
            return D3D11FrameCopy::SameObject(device, producerDevice.Get());
        }

        std::atomic<DWORD> thread_{};
        NativeGuideSnapshot snapshot_;
        std::uint64_t observedPresent_{};
        bool valid_{}, observed_{};
    };

    // Used by the engine thunk and fixture: publication precedes the host's
    // native-UI option guard, including when evaluation is deferred to Present.
    template<class Host>
    void ObserveNativeWorldCompletion(Host& host, bool mainOrLoading)
    {
        host.ObserveWorldGuides(mainOrLoading);
        host.OnBackgroundReady(BackgroundBoundary::World, mainOrLoading);
    }
}
