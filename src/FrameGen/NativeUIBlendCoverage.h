#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <vector>

namespace TheosRenderPipeline
{
    enum class UIBlendCoverage { Unchanged, SourceOver, Additive };

    // A transparent UI layer composes as Final = UI + (1 - a) * scene, so its
    // stored alpha must be coverage. Producers written for an opaque back buffer
    // never see their stored alpha: source-over with alpha factors SRC_ALPHA /
    // INV_SRC_ALPHA stores a*a, and IED 1.7.4's ImGui backend (INV_SRC_ALPHA /
    // ZERO) stores a*(1-a). Source-over gets a + prior*(1-a); additive light does
    // not occlude and keeps prior coverage. RGB factors are never changed. Logic
    // operations, partial write masks, alpha-to-coverage, unblended and other
    // equations are left alone. Only render target 0 (the UI layer) is changed.
    inline UIBlendCoverage CorrectCoverage(D3D11_BLEND_DESC& desc, bool logicOp)
    {
        auto& rt = desc.RenderTarget[0];
        if (logicOp || desc.AlphaToCoverageEnable || !rt.BlendEnable ||
            rt.RenderTargetWriteMask != D3D11_COLOR_WRITE_ENABLE_ALL || rt.BlendOp != D3D11_BLEND_OP_ADD ||
            (rt.SrcBlend != D3D11_BLEND_SRC_ALPHA && rt.SrcBlend != D3D11_BLEND_ONE) ||
            (rt.DestBlend != D3D11_BLEND_INV_SRC_ALPHA && rt.DestBlend != D3D11_BLEND_ONE)) {
            return UIBlendCoverage::Unchanged;
        }
        const bool additive = rt.DestBlend == D3D11_BLEND_ONE;
        const auto src = additive ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
        if (rt.SrcBlendAlpha == src && rt.DestBlendAlpha == rt.DestBlend && rt.BlendOpAlpha == D3D11_BLEND_OP_ADD) {
            return UIBlendCoverage::Unchanged; // Already coverage.
        }
        rt.SrcBlendAlpha = src;
        rt.DestBlendAlpha = rt.DestBlend;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        return additive ? UIBlendCoverage::Additive : UIBlendCoverage::SourceOver;
    }

    // Binds a coverage-correct equivalent of the producer's blend state only while
    // render target 0 is the UI layer, and the producer's own state otherwise: the
    // engine's state cache may skip re-binding a state it believes is current, so
    // a corrected state must not outlive the UI layer binding. The producer's state
    // is tracked directly rather than mapped back from the corrected one, because
    // D3D11 returns one object for identical descriptions. Render thread only.
    class NativeUIBlendCoverage
    {
    public:
        struct Result
        {
            ID3D11BlendState* state;
            UIBlendCoverage kind;
            bool firstSeen; // The producer state was not seen on the UI layer before.
        };
        Result Corrected(ID3D11Device* device, ID3D11BlendState* original)
        {
            if (!device || !original) { return {original, UIBlendCoverage::Unchanged, false}; }
            if (const auto* pair = Find(original)) { return {pair->corrected ? pair->corrected.Get() : original, pair->kind, false}; }
            if (pairs_.size() >= kMaximumStates) { return {original, UIBlendCoverage::Unchanged, false}; }
            D3D11_BLEND_DESC desc{};
            original->GetDesc(&desc);
            bool logicOp = false;
            Microsoft::WRL::ComPtr<ID3D11BlendState1> extended;
            if (SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&extended)))) {
                D3D11_BLEND_DESC1 detail{};
                extended->GetDesc1(&detail);
                logicOp = detail.RenderTarget[0].LogicOpEnable;
            }
            Pair pair{original};
            pair.kind = CorrectCoverage(desc, logicOp);
            if (pair.kind != UIBlendCoverage::Unchanged && FAILED(device->CreateBlendState(&desc, &pair.corrected))) {
                pair.kind = UIBlendCoverage::Unchanged;
            }
            pairs_.push_back(pair);
            return {pair.corrected ? pair.corrected.Get() : original, pair.kind, true};
        }
        // A producer's OMSetBlendState: remember its state; returns the state to bind.
        Result Producer(ID3D11Device* device, ID3D11BlendState* state, bool uiTarget)
        {
            producer_ = state;
            const auto result = uiTarget ? Corrected(device, state) : Result{state, UIBlendCoverage::Unchanged, false};
            correctedBound_ = result.state != state;
            return result;
        }
        // Host passes set and restore their own states (sometimes our corrected
        // one, read back by OMGetBlendState); none of them is the producer's.
        void Internal(ID3D11BlendState* state) { correctedBound_ = state != producer_.Get(); }
        // After a render-target change: bind the corrected state while target 0 is
        // uiTarget, otherwise the producer's own. setBlend must bypass any hook.
        template<class SetBlend>
        void Sync(ID3D11DeviceContext* context, ID3D11RenderTargetView* uiTarget, SetBlend&& setBlend)
        {
            if (!context) { return; }
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> first;
            context->OMGetRenderTargets(1, &first, nullptr);
            Microsoft::WRL::ComPtr<ID3D11BlendState> current;
            FLOAT factors[4]{};
            UINT mask = 0;
            context->OMGetBlendState(&current, factors, &mask);
            // ClearState or a context-state swap changed it without a setter call.
            if (current.Get() != producer_.Get() && current.Get() != CorrectedOf(producer_.Get()) && !AnyCorrected(current.Get())) {
                producer_ = current;
            }
            auto* wanted = producer_.Get();
            if (uiTarget && first.Get() == uiTarget) {
                Microsoft::WRL::ComPtr<ID3D11Device> device;
                context->GetDevice(&device);
                wanted = Corrected(device.Get(), producer_.Get()).state;
            }
            if (wanted != current.Get()) { setBlend(wanted, factors, mask); }
            correctedBound_ = wanted != producer_.Get();
        }
        // Whether a render-target change must re-check the blend state: during the
        // UI pass, including its entry bind, or while a corrected state is bound.
        bool NeedsSync(bool uiPassActive) const { return uiPassActive || correctedBound_; }
        // A corrected state is bound, so a render-target change must re-check it.
        bool CorrectedBound() const { return correctedBound_; }
        std::size_t CorrectedCount() const
        {
            std::size_t count = 0;
            for (const auto& pair : pairs_) { count += pair.corrected != nullptr; }
            return count;
        }
        void Reset() { pairs_.clear(); producer_.Reset(); correctedBound_ = false; }

    private:
        static constexpr std::size_t kMaximumStates = 256;
        struct Pair
        {
            Microsoft::WRL::ComPtr<ID3D11BlendState> original, corrected;
            UIBlendCoverage kind{UIBlendCoverage::Unchanged};
        };
        const Pair* Find(ID3D11BlendState* original) const
        {
            for (const auto& pair : pairs_) { if (pair.original.Get() == original) { return &pair; } }
            return nullptr;
        }
        ID3D11BlendState* CorrectedOf(ID3D11BlendState* original) const
        {
            const auto* pair = original ? Find(original) : nullptr;
            return pair && pair->corrected ? pair->corrected.Get() : original;
        }
        bool AnyCorrected(ID3D11BlendState* state) const
        {
            for (const auto& pair : pairs_) { if (pair.corrected && pair.corrected.Get() == state) { return true; } }
            return false;
        }
        std::vector<Pair> pairs_;
        Microsoft::WRL::ComPtr<ID3D11BlendState> producer_;
        bool correctedBound_{};
    };
}
