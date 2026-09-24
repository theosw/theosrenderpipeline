#include "PerformanceTuning.h"
#include "FrameTrace.h"
#include "VideoMemoryTelemetry.h"

#include <chrono>
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

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    Require(argc == 2, "choose --cpu or --warp");
    if (std::string_view(argv[1]) == "--cpu") { CPUChecks(); }
    else { Require(std::string_view(argv[1]) == "--warp", "known mode"); WARPChecks(); }
}
