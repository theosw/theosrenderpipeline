// Execute the production world's complete first-frame/recreate control flow.
// Only device operations and the vendor pass are doubles; no GPU is used.
#include <Windows.h>
#include <dxgiformat.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <utility>
#include "FrameGen/SourceDLSSGNeuralState.h"
#include "FrameGen/NeuralAdmission.h"

static void Require(bool value, const char* reason) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
struct ID3D11Texture2D {};
struct ID3D12GraphicsCommandList {};
namespace sl {
enum Boolean { eFalse, eTrue };
struct Constants { Boolean reset{}, depthInverted{}; struct XY { float x{}, y{}; } mvecScale, jitterOffset; };
}
struct PerformanceTuning {
    enum class D3D11Stage { kNeuralEarlyRoundTrip };
    static PerformanceTuning* GetSingleton() { static PerformanceTuning p; return &p; }
    bool TimingEnabled() const { return false; }
    void RecordNeuralEarlyCPU(std::uint64_t, std::uint64_t, bool) {}
};
struct ScopedD3D11PerformanceStage { ScopedD3D11PerformanceStage(void*, PerformanceTuning::D3D11Stage) {} };
namespace TheosRenderPipeline::SourceDLSSG {
struct FrameExtent { unsigned width{}, height{}; };
enum class Work { Upscaling };
struct AllocatorWaitTiming { std::uint64_t nanoseconds{}; bool waited{}; };
struct Pointer { void* Get() const { return nullptr; } };
struct Guide { Pointer texture11, texture12; struct { unsigned Width{64}, Height{64}; } desc; };
struct Interop {
    unsigned discards{}, submits{};
    template<class... T> HRESULT CopyInputRegion(T...) { return S_OK; }
    HRESULT SignalD3D11(Work) { return S_OK; }
    HRESULT Begin(Work, ID3D12GraphicsCommandList** list, AllocatorWaitTiming*, bool) { *list=&commands; return S_OK; }
    unsigned CurrentSlot(Work) { return 0; }
    void* Marks(Work) { return nullptr; }
    HRESULT Discard(Work) { ++discards; return S_OK; }
    ID3D12GraphicsCommandList* CurrentList(Work) { return &commands; }
    static HRESULT RecordCopy(ID3D12GraphicsCommandList*, void*, void*) { return S_OK; }
    HRESULT Submit(Work) { ++submits; return S_OK; }
    HRESULT WaitD3D11(Work) { return S_OK; }
    ID3D12GraphicsCommandList commands;
};
struct D3D11FrameCopy { static HRESULT Color(void*, void*, ID3D11Texture2D*, FrameExtent) { return S_OK; } };
struct NeuralPass {
    static inline unsigned created{}, recorded{};
    static inline bool nextPreparing{}, reject{};
    volatile bool preparing{}; // RC1's null member read must really execute.
    NeuralPass() { ++created; }
    bool Preparing() const { return preparing; }
    bool ModelChangePending(const NeuralOptions&) const { return false; }
    bool NeedsRecreation(const NeuralOptions&, unsigned, unsigned) const { return false; }
    template<class... T> bool Record(T&&...) { ++recorded; preparing=nextPreparing; return !preparing && !reject; }
    void* Corrected() const { return nullptr; }
    bool Submitted(void*) const { return true; }
    bool AMDFailed() const { return false; }
};
struct Backend {
    bool ready{true}, recreate{}, retry{}, unavailable{};
    unsigned retirements{};
    std::unique_ptr<NeuralPass> neuralPass_;
    NeuralOptions frameNeuralOptions_;
    NeuralHistory neuralHistory_;
    struct Recovery { bool disabled{}; bool Disabled() const { return disabled; } } neuralRecovery_;
    struct Capability { bool Pascal() const { return false; } } neuralCapability_;
    struct { struct { unsigned Width{64}, Height{64}; } BufferDesc; } presenterDesc_;
    struct { bool active{}; std::string status; } neuralSnapshot_;
    std::atomic_bool neuralTransitionBlocked_{};
    std::mutex neuralMutex_;
    bool neuralFrameBegun_{}, neuralEvaluatedEarly_{}, frameNeuralModelChanged_{}, neuralEligible_{};
    bool frameNeuralReset_{}, neuralRecoveryReset_{}, objectMaskCurrent_{};
    int neuralComputeCapability_{}, fault_{};
    std::uint64_t neuralTimestampFrequency_{};
    Pointer device12_, context11_, queue_;
    Guide motion_, depth_, earlyNeuralColor_, objectMask_;
    Interop interop_;
    bool Ready() const { return ready; }
    void ApplyNeuralRetry(const NeuralOptions&, FrameExtent) { if (std::exchange(retry,false)) { neuralRecovery_.disabled=false; neuralRecoveryReset_=true; } }
    const char* NeuralUnavailableReason(const NeuralOptions&, FrameExtent) { return unavailable || neuralRecovery_.Disabled() ? "refused" : nullptr; }
    bool RecreateNeuralIfNeeded(const NeuralOptions&, FrameExtent = {}) {
        if (std::exchange(recreate,false)) { neuralPass_.reset(); ++retirements; } return true;
    }
    bool EnsureGuide(ID3D11Texture2D*, Guide&, DXGI_FORMAT, FrameExtent, bool) { return true; }
    bool CopyObjectMask(FrameExtent) { return true; }
    bool CopyDepth(ID3D11Texture2D*) { return true; }
    bool Check(HRESULT hr, const char*) { return SUCCEEDED(hr); }
    bool FailNeuralRecording(const char* = "") { neuralPass_.reset(); neuralRecovery_.disabled=true; return true; }
    unsigned FrameIndex() const { return 1; }
    bool EvaluateNeuralBeforeUpscaling(const NeuralOptions&, const sl::Constants*, bool, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*, FrameExtent, bool&);
    bool EvaluateNeuralWorld(const NeuralOptions&, const sl::Constants*, bool, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*, FrameExtent, FrameExtent, bool&);
};
#ifndef TRP_NEURAL_WORLD_STAGE_SOURCE
#define TRP_NEURAL_WORLD_STAGE_SOURCE "FrameGen/SourceDLSSGNeuralWorld.inl"
#endif
#include TRP_NEURAL_WORLD_STAGE_SOURCE
}
static void Run() {
    using namespace TheosRenderPipeline;
    using namespace SourceDLSSG;
    for (const auto selected : {NeuralRendering::Backend::Native, NeuralRendering::Backend::HIP, NeuralRendering::Backend::Vulkan}) {
        Backend b; NeuralOptions o; o.enabled=true; o.backend=selected; o.beforeUpscaling=true;
        sl::Constants camera; bool reset{};
        auto frame = [&] { reset=false; return b.EvaluateNeuralBeforeUpscaling(o,&camera,true,nullptr,nullptr,nullptr,{64,64},reset); };
        const auto first=NeuralPass::created;
        Require(frame() && b.neuralPass_ && b.neuralEvaluatedEarly_, "first frame creates and evaluates the pass");
        Require(NeuralPass::created==first+1 && b.interop_.submits==1, "first frame submits once");
        Require(frame() && !reset && NeuralPass::created==first+1, "steady frame reuses pass without resetting");
        b.recreate=true;
        Require(frame() && b.retirements==1 && NeuralPass::created==first+2, "Apply retires and recreates before recording");
        NeuralPass::reject=true;
        Require(frame() && !b.neuralPass_ && b.neuralRecovery_.Disabled(), "refusal retires pass");
        NeuralPass::reject=false; b.retry=true;
        Require(frame() && b.neuralPass_ && reset, "recovery Apply recreates pass");
        b.recreate=true; NeuralPass::nextPreparing=true;
        Require(frame() && b.neuralPass_->Preparing() && !b.neuralEvaluatedEarly_ && b.interop_.discards==1, "first preparing frame discards list");
        Require(frame() && !reset && b.interop_.discards==2, "preparing frame preserves upscaler history");
        NeuralPass::nextPreparing=false;
        Require(frame() && reset && b.neuralEvaluatedEarly_, "preparing to ready resets upscaler history");
        std::printf("PASS: %s first/steady/Apply/recovery/preparing/ready stage sequence\n", NeuralRendering::BackendName(selected));
    }
}
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    __try { Run(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { std::fprintf(stderr,"FAIL: stage raised exception 0x%08lx\n",GetExceptionCode()); return 1; }
    return 0;
}
