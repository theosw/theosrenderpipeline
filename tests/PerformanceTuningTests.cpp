#include "PerformanceTuning.h"
#include "FrameTrace.h"
#include "VideoMemoryTelemetry.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using Stage = PerformanceTuning::D3D11Stage;
extern "C" ID3D11DeviceContext* TRPTimingContextAlias(ID3D11DeviceContext*);
extern "C" unsigned TRPTimingContextAliasEnds();
extern "C" unsigned TRPTimingContextAliasInspections();
extern "C" void TRPTimingScriptFrame(unsigned, HRESULT, UINT64, BOOL, UINT64, UINT64);
extern "C" void TRPTimingScriptEdge(unsigned, int, HRESULT);
extern "C" unsigned TRPTimingScriptPolls(unsigned);
extern "C" unsigned TRPTimingScriptEdgePolls(unsigned, unsigned);
extern "C" unsigned TRPTimingScriptCalls();
extern "C" unsigned TRPTimingScriptFrames();
extern "C" BOOL TRPTimingScriptReady(unsigned);
extern "C" BOOL TRPTimingScriptEdgeReady(unsigned, unsigned);
static constexpr auto Index(Stage stage) { return static_cast<std::size_t>(stage); }
static void Require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void Check(HRESULT hr, const char* message) { Require(SUCCEEDED(hr), message); }

// Link-time doubles for unrelated services. Query creation/submission/readback,
// frame lifecycle, scope ownership, sample publication and RAII are production.
static std::vector<FrameTrace::TraceEventV1> events;
FrameTrace::FrameTrace() = default;
FrameTrace::~FrameTrace() = default;
bool FrameTrace::SetEnabled(bool enabled) { enabled_.store(enabled); return true; }
bool FrameTrace::Record(EventType type, std::uint16_t flags, std::int64_t qpc,
    std::uint64_t frame, std::uint64_t correlation, std::uint16_t numerator,
    std::uint16_t denominator, std::int64_t arg0, std::int64_t arg1)
{
    if (!Enabled()) { return false; }
    events.push_back({static_cast<std::uint16_t>(type), flags, sizeof(TraceEventV1),
        0, qpc, frame, correlation, 0, numerator, denominator, arg0, arg1});
    return true;
}
void VideoMemoryTelemetry::Update() {}

static auto& Timing() { return *PerformanceTuning::GetSingleton(); }
static void RouteCPUChecks()
{
    using Route = PerformanceTuning::Optimization;
    constexpr std::array routes{Route::kDirectRCASOutput, Route::kDirectDLSSOutput};
    auto& p = Timing();
    p.ApplySettings({false, false, true, true});
    Require(!p.TimingEnabled(), "route bookkeeping does not require timings or tracing");
    p.BeginRouteFrame();
    for (auto route : routes) {
        p.MarkRouteEligible(route, "compatible target");
        p.MarkRouteActive(route);
        const auto& status = p.GetRouteStatus(route);
        Require(status.requested && status.eligible && status.activeLastFrame && status.activeFrames == 1,
            "actual execution activates a requested route with timings disabled");
    }
    p.BeginD3D11Frame(nullptr, nullptr);
    p.EndD3D11Frame(nullptr);
    for (auto route : routes) {
        Require(p.GetRouteStatus(route).activeLastFrame, "disabled timing callbacks do not erase current execution");
    }
    p.BeginRouteFrame();
    for (auto route : routes) {
        const auto& status = p.GetRouteStatus(route);
        Require(status.requested && status.eligible && !status.activeLastFrame && status.activeFrames == 1 &&
            status.fallbackCount == 0 && !status.sessionRejected,
            "new frame clears only execution while retaining requests, capability and history");
    }
    // A skipped evaluation does not reactivate the route or increment history.
    p.BeginRouteFrame();
    for (auto route : routes) {
        Require(!p.GetRouteStatus(route).activeLastFrame && p.GetRouteStatus(route).activeFrames == 1,
            "skipped frame remains inactive without adding activity");
    }

    p.ApplySettings({true, false, true, true});
    for (auto route : routes) { p.MarkRouteActive(route); }
    p.BeginD3D11Frame(nullptr, nullptr);
    Require(!p.BeginD3D11Stage(nullptr, Stage::kOutputCopy) && !p.EndD3D11Stage(nullptr, {}),
        "null timing callbacks remain invalid");
    p.EndD3D11Frame(nullptr);
    for (auto route : routes) {
        Require(p.GetRouteStatus(route).activeLastFrame && p.GetRouteStatus(route).activeFrames == 2,
            "invalid timing callbacks cannot erase successful current-frame execution");
    }
    p.BeginRouteFrame();
    p.BeginD3D11Frame(nullptr, nullptr);
    for (auto route : routes) {
        Require(!p.GetRouteStatus(route).activeLastFrame && p.GetRouteStatus(route).activeFrames == 2,
            "real route boundary clears execution even when timing cannot start");
    }
    Require(p.GetTimingSnapshot().d3d11Samples == 0, "CPU route checks create no GPU measurements");

    p.MarkRouteFallback(Route::kDirectRCASOutput, "unsupported target", true);
    p.MarkRouteWaiting(Route::kDirectDLSSOutput, "loading reconstruction");
    p.BeginRouteFrame();
    const auto& rejected = p.GetRouteStatus(Route::kDirectRCASOutput);
    const auto& waiting = p.GetRouteStatus(Route::kDirectDLSSOutput);
    Require(rejected.requested && rejected.sessionRejected && !rejected.eligible && !rejected.activeLastFrame &&
        rejected.activeFrames == 2 && rejected.fallbackCount == 1 && rejected.reason == "unsupported target" &&
        !p.IsRouteAllowed(Route::kDirectRCASOutput), "frame reset preserves session fallback and cumulative history");
    Require(waiting.requested && !waiting.eligible && !waiting.activeLastFrame && waiting.activeFrames == 2 &&
        waiting.fallbackCount == 0 && waiting.reason == "loading reconstruction", "frame reset preserves useful waiting reason");
    p.ApplySettings({false, false, true, false});
    p.BeginRouteFrame();
    Require(!waiting.requested && !waiting.eligible && !waiting.activeLastFrame && waiting.reason == "disabled" &&
        waiting.activeFrames == 2 && rejected.sessionRejected && rejected.fallbackCount == 1,
        "disabled route stays off while counters and other route's latch survive");
    std::puts("PASS: production route state with timings off; skipped frames; execution until next boundary; invalid timing independence; preserved capability, requests, reasons, counters and fallback latch (CPU only; hook placement source-reviewed)");
}

static void CPUChecks()
{
    static_assert(Index(Stage::kFrame) == 0 && Index(Stage::kFrameGenInputs) == 1 &&
        Index(Stage::kInputColorCopy) == 2 && Index(Stage::kMaskEncode) == 3 &&
        Index(Stage::kDLSS) == 4 && Index(Stage::kRCAS) == 5 && Index(Stage::kOutputCopy) == 6 &&
        Index(Stage::kHUDLessCopy) == 7 && Index(Stage::kNativeUIComposition) == 8 &&
        Index(Stage::kStartupOverlayComposition) == 9 && Index(Stage::kNeuralEarlyRoundTrip) == 10 &&
        Index(Stage::kPresentationCopy) == 11);
    auto& p = Timing();
    p.ApplySettings({false, false, false, false});
    p.BeginD3D11Frame(nullptr, nullptr);
    Require(!p.BeginD3D11Stage(nullptr, Stage::kOutputCopy), "disabled start has no owner");
    Require(!p.EndD3D11Stage(nullptr, {}), "invalid ticket cannot end");
    { ScopedD3D11PerformanceStage scope{nullptr, Stage::kOutputCopy}; }
    p.EndD3D11Frame(nullptr);
    Require(p.GetTimingSnapshot().d3d11Samples == 0, "disabled path publishes no sample");
    Require(p.GetScopeDiagnostics().rejectedEnds == 0, "rejected RAII start has no End side effect");
    Require(p.GetScopeDiagnostics().contextMismatches == 0, "inactive scopes are not misuse");
    std::puts("PASS: stable stage IDs; disabled/no-owner lifecycle (CPU only)");
    RouteCPUChecks();
}

struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Device()
    {
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
    }
};

static std::uint64_t nextFrame = 1;
static std::uint64_t Start(Device& d)
{
    Timing().ApplySettings({true, true, false, false});
    Timing().ResetTimingWindow();
    events.clear();
    const auto frame = nextFrame++;
    Timing().BeginD3D11Frame(d.device.Get(), d.context.Get(), frame);
    return frame;
}
static PerformanceTuning::TimingSnapshot Retire(Device& d)
{
    const auto before = Timing().GetTimingSnapshot().d3d11Samples;
    // Stop creating frames while retaining this epoch for readback assertions.
    // ApplySettings deliberately resets the epoch; that path is tested below.
    const auto settings = Timing().settings;
    Timing().settings.enableGPUTimings = false;
    Timing().settings.enableFrameTrace = false;
    // This headless fixture has no Present to submit commands. Only the harness
    // flushes/yields; production continues using bounded DONOTFLUSH polling.
    d.context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        Timing().BeginD3D11Frame(d.device.Get(), d.context.Get(), nextFrame++);
        Timing().EndD3D11Frame(d.context.Get());
        if (Timing().GetTimingSnapshot().d3d11Samples > before) {
            const auto snapshot = Timing().GetTimingSnapshot();
            Timing().settings = settings;
            return snapshot;
        }
        Require(std::chrono::steady_clock::now() < deadline, "bounded WARP query retirement");
        d.context->Flush();
        Sleep(1);
    } while (true);
}
static unsigned Traces(std::uint64_t frame, Stage stage)
{
    unsigned count = 0;
    for (const auto& e : events) {
        if (e.type == static_cast<std::uint16_t>(FrameTrace::EventType::kGpuStage) &&
            e.realFrameId == frame && e.phaseNumerator == Index(stage)) {
            Require(e.flags == FrameTrace::kSuccess && e.phaseDenominator == Index(Stage::kCount),
                "trace stage contract preserved");
            ++count;
        }
    }
    return count;
}
static void Valid(const PerformanceTuning::TimingSnapshot& s, Stage stage)
{
    Require(s.d3d11Available[Index(stage)] && !s.d3d11ScopeInvalid[Index(stage)], "valid scope published");
}
static void Invalid(const PerformanceTuning::TimingSnapshot& s, Stage stage)
{
    Require(!s.d3d11Available[Index(stage)] && s.d3d11ScopeInvalid[Index(stage)], "invalid scope suppressed");
}
static auto Begin(Device& d, Stage stage) { return Timing().BeginD3D11Stage(d.context.Get(), stage); }
static bool End(Device& d, PerformanceTuning::D3D11StageTicket ticket)
{ return Timing().EndD3D11Stage(d.context.Get(), ticket); }
static void Finish(Device& d) { Timing().EndD3D11Frame(d.context.Get()); }

static void WARPChecks()
{
    Device a, b;
    auto& p = Timing();
    ComPtr<ID3D11DeviceContext> deferred;
    Check(a.device->CreateDeferredContext(0, &deferred), "deferred context for rejection");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 8; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    ComPtr<ID3D11Texture2D> input, handoff, output;
    unsigned pixels[64]; for (auto& pixel : pixels) { pixel = 0xff123456; }
    D3D11_SUBRESOURCE_DATA data{pixels, 8 * sizeof(unsigned), 0};
    Check(a.device->CreateTexture2D(&desc, &data, &input), "input texture");
    Check(a.device->CreateTexture2D(&desc, nullptr, &handoff), "handoff texture");
    Check(a.device->CreateTexture2D(&desc, nullptr, &output), "presentation texture");
    auto frame = Start(a);
    {
        ScopedD3D11PerformanceStage scope{a.context.Get(), Stage::kOutputCopy};
        a.context->CopyResource(handoff.Get(), input.Get());
    }
    {
        ScopedD3D11PerformanceStage scope{a.context.Get(), Stage::kPresentationCopy};
        a.context->CopyResource(output.Get(), handoff.Get());
    }
    Finish(a);
    auto snapshot = Retire(a);
    Valid(snapshot, Stage::kOutputCopy); Valid(snapshot, Stage::kPresentationCopy);
    Require(Traces(frame, Stage::kOutputCopy) == 1 && Traces(frame, Stage::kPresentationCopy) == 1,
        "both physical copies publish distinct trace IDs");
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    Check(a.device->CreateTexture2D(&desc, nullptr, &readback), "copy readback");
    a.context->CopyResource(readback.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(a.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback map");
    Require(*static_cast<unsigned*>(mapped.pData) == pixels[0], "two copies retain source pixel");
    a.context->Unmap(readback.Get(), 0);

    frame = Start(a); // Simulates omission of backend copy, not an NVIDIA route test.
    { ScopedD3D11PerformanceStage scope{a.context.Get(), Stage::kPresentationCopy}; }
    Finish(a); snapshot = Retire(a);
    Require(!snapshot.d3d11Available[Index(Stage::kOutputCopy)] && !snapshot.d3d11ScopeInvalid[Index(Stage::kOutputCopy)],
        "absent backend is unavailable, not invalid or a borrowed host measurement");
    Valid(snapshot, Stage::kPresentationCopy);
    Require(Traces(frame, Stage::kOutputCopy) == 0, "absent stage emits no success");

    Start(a);
    {
        ScopedD3D11PerformanceStage outer{a.context.Get(), Stage::kDLSS};
        ScopedD3D11PerformanceStage inner{a.context.Get(), Stage::kRCAS};
    }
    Finish(a); snapshot = Retire(a); Valid(snapshot, Stage::kDLSS); Valid(snapshot, Stage::kRCAS);

    frame = Start(a);
    auto* alias = TRPTimingContextAlias(a.context.Get());
    Require(alias != a.context.Get(), "forwarder has a distinct interface pointer");
    auto aliasOwner = p.BeginD3D11Stage(alias, Stage::kDLSS);
    Require(static_cast<bool>(aliasOwner), "compatible wrapper can acquire its own stage");
    Require(!p.EndD3D11Stage(a.context.Get(), aliasOwner), "underlying interface cannot end wrapper's ticket");
    Require(TRPTimingContextAliasEnds() == 1, "wrong-interface End emits no timestamp");
    Require(p.EndD3D11Stage(alias, aliasOwner), "wrapper ends its own ticket");
    Require(TRPTimingContextAliasEnds() == 2 && TRPTimingContextAliasInspections() == 0,
        "compatible wrapper needs only two timestamps, no stage QI/GetDevice/GetType");
    Finish(a); snapshot = Retire(a); Valid(snapshot, Stage::kDLSS);
    Require(Traces(frame, Stage::kDLSS) == 1, "wrapper stage retires on the owning frame context");

    Start(a);
    const auto beforeAliasFrame = Begin(a, Stage::kDLSS);
    p.BeginD3D11Frame(a.device.Get(), alias, nextFrame++);
    auto nativeOwner = Begin(a, Stage::kRCAS);
    Require(static_cast<bool>(nativeOwner) && !End(a, beforeAliasFrame), "frame interface replacement keeps generations unique");
    Require(!p.EndD3D11Stage(alias, nativeOwner) && End(a, nativeOwner), "native stage owns its interface within wrapped frame");
    p.EndD3D11Frame(a.context.Get()); // Rejected; only the retained frame owner closes it.
    p.EndD3D11Frame(alias);
    snapshot = Retire(a); Valid(snapshot, Stage::kRCAS); Valid(snapshot, Stage::kFrame);

    frame = Start(a);
    auto before = p.GetScopeDiagnostics();
    auto owner = Begin(a, Stage::kOutputCopy);
    Require(static_cast<bool>(owner), "outer acquires owner");
    { ScopedD3D11PerformanceStage rejected{a.context.Get(), Stage::kOutputCopy}; }
    Require(End(a, owner), "rejected nested RAII destructor did not end outer");
    Finish(a); snapshot = Retire(a); Invalid(snapshot, Stage::kOutputCopy); Valid(snapshot, Stage::kFrame);
    Require(p.GetScopeDiagnostics().conflictingStarts == before.conflictingStarts + 1 &&
        p.GetScopeDiagnostics().unclosedScopes == before.unclosedScopes, "nested reuse diagnosed without false unclosed");
    Require(Traces(frame, Stage::kOutputCopy) == 0, "conflicting pair emits no success");

    frame = Start(a); before = p.GetScopeDiagnostics();
    owner = Begin(a, Stage::kOutputCopy); Require(End(a, owner), "first sequential interval closes");
    Require(!Begin(a, Stage::kOutputCopy), "sequential reuse rejected");
    Finish(a); snapshot = Retire(a); Invalid(snapshot, Stage::kOutputCopy);
    Require(p.GetScopeDiagnostics().conflictingStarts == before.conflictingStarts + 1 && Traces(frame, Stage::kOutputCopy) == 0,
        "sequential reuse invalidates even the first completed pair");

    frame = Start(a); before = p.GetScopeDiagnostics();
    owner = Begin(a, Stage::kRCAS); Finish(a); snapshot = Retire(a);
    Invalid(snapshot, Stage::kRCAS);
    Require(p.GetScopeDiagnostics().unclosedScopes == before.unclosedScopes + 1 && Traces(frame, Stage::kRCAS) == 0,
        "frame boundary does not synthesize a successful scope end");

    Start(a);
    std::optional<ScopedD3D11PerformanceStage> stale;
    stale.emplace(a.context.Get(), Stage::kDLSS);
    Finish(a);
    Start(a); owner = Begin(a, Stage::kDLSS); before = p.GetScopeDiagnostics();
    stale.reset();
    Require(p.GetScopeDiagnostics().rejectedEnds == before.rejectedEnds + 1 && End(a, owner),
        "old-frame RAII destructor cannot end new frame");
    Finish(a); snapshot = Retire(a); Valid(snapshot, Stage::kDLSS);

    Start(a); const auto old = Begin(a, Stage::kOutputCopy); Require(End(a, old), "old ticket closes");
    Finish(a); Retire(a);
    for (unsigned i = 0; i < 5; ++i) { Start(a); Finish(a); Retire(a); }
    Start(a); owner = Begin(a, Stage::kOutputCopy);
    Require(!End(a, old) && End(a, owner), "ticket cannot survive epoch resets and ring wrap");
    Finish(a); snapshot = Retire(a); Valid(snapshot, Stage::kOutputCopy);

    Start(a); owner = Begin(a, Stage::kDLSS); before = p.GetScopeDiagnostics();
    Require(!p.BeginD3D11Stage(nullptr, Stage::kDLSS), "null start has no owner");
    Require(!p.EndD3D11Stage(b.context.Get(), owner), "wrong context end rejected");
    Require(!p.EndD3D11Stage(nullptr, owner), "null context cannot end owner");
    p.EndD3D11Frame(b.context.Get());
    p.BeginD3D11Frame(a.device.Get(), deferred.Get(), nextFrame++);
    Require(p.GetScopeDiagnostics().contextMismatches == before.contextMismatches + 4 && End(a, owner),
        "wrong End contexts and deferred frame input do not close owner");
    Finish(a); snapshot = Retire(a); Valid(snapshot, Stage::kDLSS);

    Start(a); owner = Begin(a, Stage::kDLSS); before = p.GetScopeDiagnostics();
    // Do not close old frame: switching devices must use its retained context.
    p.BeginD3D11Frame(b.device.Get(), b.context.Get(), nextFrame++);
    auto newOwner = Begin(b, Stage::kDLSS);
    Require(static_cast<bool>(newOwner) && !End(a, owner) && End(b, newOwner), "device replacement rejects old generation");
    Require(p.GetScopeDiagnostics().unclosedScopes == before.unclosedScopes + 1, "device replacement diagnoses open old scope");
    Finish(b); snapshot = Retire(b); Valid(snapshot, Stage::kDLSS);
    Start(a); newOwner = Begin(a, Stage::kDLSS);
    Require(!End(a, owner) && End(a, newOwner), "returning to same device/context cannot revive old ticket");
    Finish(a); snapshot = Retire(a); Valid(snapshot, Stage::kDLSS);

    Start(a); before = p.GetScopeDiagnostics();
    Require(!Begin(a, Stage::kCount), "invalid stage rejected");
    Require(p.GetScopeDiagnostics().invalidStages == before.invalidStages + 1, "invalid stage diagnosed");
    owner = Begin(a, Stage::kRCAS); Require(End(a, owner) && !End(a, owner), "double End rejected");
    Finish(a); snapshot = Retire(a); Invalid(snapshot, Stage::kRCAS);

    Start(a); owner = Begin(a, Stage::kNeuralEarlyRoundTrip);
    p.RecordNeuralEarlyCPU(500, 100, true);
    Require(!Begin(a, Stage::kNeuralEarlyRoundTrip), "NR conflict detected");
    Require(End(a, owner), "NR owner still closes"); Finish(a); snapshot = Retire(a);
    Invalid(snapshot, Stage::kNeuralEarlyRoundTrip);
    Require(snapshot.neuralEarly.samples == 0 && snapshot.neuralEarly.cpuNanoseconds == 0,
        "invalid GPU scope cannot contaminate paired NR totals");

    Start(a); owner = Begin(a, Stage::kOutputCopy); Require(End(a, owner), "pending sample closes"); Finish(a);
    p.ApplySettings({false, false, false, false}); // Epoch reset while queries remain in flight.
    a.context->Flush();
    for (unsigned i = 0; i < 8; ++i) {
        p.BeginD3D11Frame(a.device.Get(), a.context.Get(), nextFrame++);
        Require(!Begin(a, Stage::kOutputCopy), "disabled frame has no owner while draining");
        p.EndD3D11Frame(a.context.Get()); Sleep(1);
    }
    Require(p.GetTimingSnapshot().d3d11Samples == 0, "pre-disable pending samples stay out of reset epoch");
    p.ApplySettings({false, true, false, false});
    p.BeginD3D11Frame(a.device.Get(), a.context.Get(), nextFrame++);
    owner = Begin(a, Stage::kPresentationCopy); Require(static_cast<bool>(owner) && End(a, owner), "trace-only timing remains enabled");
    Finish(a); snapshot = Retire(a); Valid(snapshot, Stage::kPresentationCopy);
    p.ApplySettings({false, false, false, false});
    std::puts("PASS: copy identities/pixels; absence; nesting; compatible wrapper and frame interface replacement; wrong-interface End rejection; duplicate/unclosed/stale scopes; ring/epoch/device reuse; deferred-frame rejection; invalid stage/double End; NR pairing; disable/reset; trace-only timing (WARP)");
}

struct PublicationFixture
{
    Device device;
    ID3D11DeviceContext* alias = TRPTimingContextAlias(device.context.Get());
    unsigned nextScript{};
    PublicationFixture()
    {
        Timing().ApplySettings({true, true, false, false});
        Timing().ResetTimingWindow();
    }
    void Configure(unsigned index, unsigned ms, HRESULT result = S_OK, UINT64 frequency = 1000000, BOOL disjoint = FALSE)
    { TRPTimingScriptFrame(index, result, frequency, disjoint, 10000, 10000 + ms * 1000); }
    std::uint64_t Submit(bool copy = true, Stage stage = Stage::kOutputCopy)
    {
        const auto frame = nextFrame++;
        Timing().BeginD3D11Frame(device.device.Get(), alias, frame);
        if (copy) {
            const auto owner = Timing().BeginD3D11Stage(alias, stage);
            Require(static_cast<bool>(owner) && Timing().EndD3D11Stage(alias, owner), "scripted scope acquisition");
        }
        Timing().EndD3D11Frame(alias);
        ++nextScript;
        return frame;
    }
    void Poll()
    {
        const auto settings = Timing().settings;
        Timing().settings.enableGPUTimings = Timing().settings.enableFrameTrace = false;
        Timing().BeginD3D11Frame(device.device.Get(), alias, nextFrame++);
        Timing().settings = settings;
    }
    template<class Predicate> void Until(Predicate done)
    {
        device.context->Flush(); // Harness submission only, never production polling.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            Poll();
            if (done()) { return; }
            Require(std::chrono::steady_clock::now() < deadline, "bounded scripted retirement");
            Sleep(1);
        } while (true);
    }
    void Complete(std::uint64_t frame)
    { Until([&] { return Timing().GetTimingSnapshot().lastCompletedFrameId == frame; }); }
};

static void Near(float actual, float expected, const char* message)
{ Require(std::abs(actual - expected) < 0.0001f, message); }

static void PublicationChecks()
{
    PublicationFixture f;
    auto& p = Timing();
    const auto value = [&](Stage stage = Stage::kOutputCopy) { return p.GetTimingSnapshot().d3d11Ms[Index(stage)]; };
    f.Configure(f.nextScript, 1, S_OK, 1000000, TRUE); f.Complete(f.Submit());
    Require(p.GetTimingSnapshot().d3d11Samples == 0 && p.GetTimingSnapshot().lastCompletedGeneration != 0 &&
        !p.GetTimingSnapshot().d3d11Available[Index(Stage::kFrame)], "first invalid completion is unavailable, not still waiting");
    // Advance to slot 3, then hold it while newer work occupies slots 0 and 1.
    for (unsigned i = 0; i < 2; ++i) { f.Configure(f.nextScript, 1); f.Complete(f.Submit()); }
    p.ResetTimingWindow(); events.clear();
    const auto oldest = f.nextScript;
    f.Configure(oldest, 10, S_FALSE); const auto frameA = f.Submit();
    f.Configure(oldest + 1, 20); const auto frameB = f.Submit();
    f.Configure(oldest + 2, 30); const auto frameC = f.Submit();
    f.Until([&] { return TRPTimingScriptPolls(oldest) > 2; });
    Require(p.GetTimingSnapshot().d3d11Samples == 0 && TRPTimingScriptPolls(oldest + 1) == 0 &&
        TRPTimingScriptPolls(oldest + 2) == 0, "oldest pending stops this pass without polling newer slots");
    f.Configure(oldest, 10); f.Complete(frameC);
    Require(p.GetTimingSnapshot().d3d11Samples == 3, "three ready frames retire once");
    Near(value(), 12.9f, "ring wrap smooths in recording order: 10, 20, 30");
    std::vector<std::uint64_t> order;
    for (const auto& e : events) {
        if (e.type == static_cast<std::uint16_t>(FrameTrace::EventType::kGpuStage) && e.phaseNumerator == Index(Stage::kOutputCopy)) {
            order.push_back(e.realFrameId);
        }
    }
    Require(order == std::vector<std::uint64_t>{frameA, frameB, frameC}, "trace publication is chronological across wrap");

    const auto previous = p.GetTimingSnapshot();
    const auto pending = f.nextScript;
    f.Configure(pending, 40, S_FALSE); const auto pendingFrame = f.Submit();
    f.Configure(pending + 1, 50); const auto afterPending = f.Submit();
    f.Until([&] { return TRPTimingScriptPolls(pending) > 1; });
    Require(p.GetTimingSnapshot().lastCompletedGeneration == previous.lastCompletedGeneration &&
        p.GetTimingSnapshot().d3d11Available[Index(Stage::kOutputCopy)], "pending keeps latest-completed identity and availability");
    Near(value(), previous.d3d11Ms[Index(Stage::kOutputCopy)], "pending does not invent a zero");
    f.Configure(pending, 40); TRPTimingScriptEdge(pending, 2, S_FALSE);
    f.Until([&] { return TRPTimingScriptEdgePolls(pending, 2) > 0; });
    Require(p.GetTimingSnapshot().lastCompletedFrameId == previous.lastCompletedFrameId &&
        TRPTimingScriptPolls(pending + 1) == 0 && Traces(pendingFrame, Stage::kFrame) == 0,
        "pending timestamp edge publishes no partial frame and stops newer polling");
    TRPTimingScriptEdge(pending, 2, S_OK); f.Complete(afterPending);

    f.Configure(f.nextScript, 1); f.Complete(f.Submit(false));
    Require(!p.GetTimingSnapshot().d3d11Available[Index(Stage::kOutputCopy)], "absent stage clears availability");
    f.Configure(f.nextScript, 70); f.Complete(f.Submit());
    Near(value(), 70.0f, "first valid after absence seeds from sample, not ten percent");
    f.Configure(f.nextScript, 90); f.Complete(f.Submit(true, Stage::kRCAS));
    Near(value(Stage::kRCAS), 90.0f, "first-ever stage seeds independently of frame sample count");

    const auto reversed = f.nextScript;
    TRPTimingScriptFrame(reversed, S_OK, 1000000, FALSE, 10000, 9999);
    const auto reversedFrame = f.Submit(); f.Complete(reversedFrame);
    Require(!p.GetTimingSnapshot().d3d11Available[Index(Stage::kOutputCopy)] && Traces(reversedFrame, Stage::kOutputCopy) == 0,
        "reversed pair has no available or successful measurement");
    f.Configure(f.nextScript, 0); f.Complete(f.Submit());
    Valid(p.GetTimingSnapshot(), Stage::kOutputCopy); Near(value(), 0.0f, "equal timestamps are a measured zero");
    f.Configure(f.nextScript, 2); f.Complete(f.Submit());
    Near(value(), 0.2f, "valid zero retains initialized smoothing history");

    auto sampleCount = p.GetTimingSnapshot().d3d11Samples;
    f.Configure(f.nextScript, 9, S_OK, 1000000, TRUE);
    const auto disjoint = f.Submit(); f.Complete(disjoint);
    Require(!p.GetTimingSnapshot().d3d11Available[Index(Stage::kFrame)] &&
        !p.GetTimingSnapshot().d3d11Available[Index(Stage::kOutputCopy)] &&
        p.GetTimingSnapshot().d3d11Samples == sampleCount && Traces(disjoint, Stage::kFrame) == 0,
        "disjoint frame invalidates prior availability without adding samples");
    f.Configure(f.nextScript, 9, S_OK, 0); f.Complete(f.Submit());
    Require(!p.GetTimingSnapshot().d3d11Available[Index(Stage::kOutputCopy)] && !p.GetQueryDiagnostics().quarantined,
        "zero frequency is unavailable, not a terminal query error");
    f.Configure(f.nextScript, 8); f.Complete(f.Submit()); Near(value(), 8.0f, "valid after disjoint/zero-frequency seeds freshly");

    const auto oldEpoch = f.nextScript;
    f.Configure(oldEpoch, 99, S_FALSE); const auto oldEpochFrame = f.Submit();
    p.ResetTimingWindow(); events.clear();
    const auto newEpoch = f.nextScript;
    f.Configure(newEpoch, 30); const auto newEpochFrame = f.Submit();
    f.Configure(oldEpoch, 99); TRPTimingScriptEdge(oldEpoch, 2, S_FALSE);
    f.Until([&] { return TRPTimingScriptEdgePolls(oldEpoch, 2) > 0; });
    Require(p.GetTimingSnapshot().d3d11Samples == 0 && TRPTimingScriptPolls(newEpoch) == 0,
        "old epoch retains its slot until recorded timestamps are ready");
    TRPTimingScriptEdge(oldEpoch, 2, S_OK); f.Complete(newEpochFrame);
    Require(p.GetTimingSnapshot().d3d11Samples == 1 && Traces(oldEpochFrame, Stage::kOutputCopy) == 0,
        "old epoch retires without invalidating or contributing to the new epoch");
    Near(value(), 30.0f, "new epoch has no old history");

    const auto completedBeforeFailure = p.GetTimingSnapshot().lastCompletedFrameId;
    const auto failedDisjoint = f.nextScript;
    f.Configure(failedDisjoint, 7, S_FALSE); f.Submit();
    f.Until([&] { return TRPTimingScriptReady(failedDisjoint) != FALSE; });
    f.Configure(failedDisjoint, 7, E_FAIL);
    auto beforeErrorFrames = TRPTimingScriptFrames(); auto beforeErrorEnds = TRPTimingContextAliasEnds();
    Require(p.TimingEnabled(), "terminal failure is discovered with sampling enabled");
    p.BeginD3D11Frame(f.device.device.Get(), f.alias, nextFrame++);
    Require(TRPTimingScriptFrames() == beforeErrorFrames && TRPTimingContextAliasEnds() == beforeErrorEnds,
        "first enabled terminal failure must not start a new frame or timestamp");
    Require(p.GetQueryDiagnostics().failure == E_FAIL && p.GetQueryDiagnostics().failures == 1 &&
        !p.GetTimingSnapshot().d3d11Available[Index(Stage::kOutputCopy)] &&
        p.GetTimingSnapshot().lastCompletedFrameId == completedBeforeFailure,
        "terminal disjoint-query failure quarantines without pretending to complete a sample");
    const auto calls = TRPTimingScriptCalls(); const auto frames = TRPTimingScriptFrames();
    p.ResetTimingWindow();
    p.ApplySettings({false, false, false, false}); f.Poll();
    p.ApplySettings({true, true, false, false});
    for (unsigned i = 0; i < 8; ++i) {
        p.BeginD3D11Frame(f.device.device.Get(), f.alias, nextFrame++);
        Require(!p.BeginD3D11Stage(f.alias, Stage::kOutputCopy), "quarantine admits no new instrumentation");
    }
    Require(p.GetQueryDiagnostics().quarantined && p.GetQueryDiagnostics().failures == 1 &&
        TRPTimingScriptCalls() == calls && TRPTimingScriptFrames() == frames,
        "window reset and toggles cannot trigger automatic failed-pair polling/recreation");
    // Ordinary D3D11 commands continue while telemetry is quarantined.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    unsigned pixel = 0xff654321; D3D11_SUBRESOURCE_DATA data{&pixel, sizeof(pixel), 0};
    ComPtr<ID3D11Texture2D> source, readback;
    Check(f.device.device->CreateTexture2D(&desc, &data, &source), "render source during quarantine");
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(f.device.device->CreateTexture2D(&desc, nullptr, &readback), "readback during quarantine");
    f.device.context->CopyResource(readback.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(f.device.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "commands survive telemetry failure");
    Require(*static_cast<unsigned*>(mapped.pData) == pixel, "copy pixels survive quarantine");
    f.device.context->Unmap(readback.Get(), 0);

    // Only pair replacement recovers. It creates fresh objects; old uncertain
    // query slots are never marked free and reused within the failed ring.
    p.BeginD3D11Frame(f.device.device.Get(), f.device.context.Get(), nextFrame++);
    auto owner = Begin(f.device, Stage::kOutputCopy);
    Require(!p.GetQueryDiagnostics().quarantined && static_cast<bool>(owner) && End(f.device, owner),
        "context-pair replacement recovers with fresh query objects");
    Finish(f.device); Valid(Retire(f.device), Stage::kOutputCopy);
    Require(p.GetQueryDiagnostics().failures == 1, "recovery retains lifetime failure diagnostic");
    f.Configure(f.nextScript, 12); f.Complete(f.Submit());
    Near(value(), 12.0f, "replacement pair starts fresh stage history");
    const auto failedEdge = f.nextScript;
    f.Configure(failedEdge, 13); TRPTimingScriptEdge(failedEdge, 2, S_FALSE);
    const auto failedEdgeFrame = f.Submit();
    f.Until([&] { return TRPTimingScriptEdgeReady(failedEdge, 2) != FALSE; });
    TRPTimingScriptEdge(failedEdge, 2, E_INVALIDARG);
    beforeErrorFrames = TRPTimingScriptFrames(); beforeErrorEnds = TRPTimingContextAliasEnds();
    p.BeginD3D11Frame(f.device.device.Get(), f.alias, nextFrame++);
    Require(TRPTimingScriptFrames() == beforeErrorFrames && TRPTimingContextAliasEnds() == beforeErrorEnds,
        "enabled timestamp failure must not start another frame or timestamp");
    Require(p.GetQueryDiagnostics().failure == E_INVALIDARG && p.GetQueryDiagnostics().failures == 2 &&
        !p.GetTimingSnapshot().d3d11Available[Index(Stage::kFrame)] && Traces(failedEdgeFrame, Stage::kFrame) == 0,
        "terminal timestamp-edge error suppresses the entire partial sample");
    Device other;
    Start(other); owner = Begin(other, Stage::kOutputCopy);
    Require(!p.GetQueryDiagnostics().quarantined && static_cast<bool>(owner) && End(other, owner), "device replacement also recovers");
    Finish(other); Valid(Retire(other), Stage::kOutputCopy);
    Require(p.GetQueryDiagnostics().failures == 2, "failure counter survives both recovery boundaries");
    std::puts("PASS: chronological wrap/trace; oldest disjoint/edge pending; latest-completed identity; independent/absent/invalid/zero smoothing; disjoint/frequency validity; old epochs; disjoint/edge hard failures; quarantine/no retry; ordinary copies; context/device replacement recovery (scripted retired WARP queries)");
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    Require(argc == 2, "choose --cpu, --warp or --publication");
    if (std::string_view(argv[1]) == "--cpu") { CPUChecks(); }
    else if (std::string_view(argv[1]) == "--publication") { PublicationChecks(); }
    else { Require(std::string_view(argv[1]) == "--warp", "known mode"); WARPChecks(); }
}
