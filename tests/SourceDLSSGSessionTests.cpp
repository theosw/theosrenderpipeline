#include "FrameGen/SourceDLSSGSession.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace TheosRenderPipeline::SourceDLSSG;

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    }
    struct Token final : sl::FrameToken
    {
        unsigned index{};
        operator std::uint32_t() const override { return index; }
    };
    struct Runtime
    {
        Token token;
        sl::DLSSGOptions submitted{};
        sl::DLSSGState state{};
        sl::Result optionsResult{sl::Result::eOk}, stateResult{sl::Result::eOk};
        sl::Result otherResult{sl::Result::eOk};
        std::string otherOperation;
        std::vector<std::string> calls;
        unsigned optionsCalls{}, waits{};
        bool waitOK{true}, lastReset{}, lastDepthInverted{};
        sl::Result Call(const char* name)
        {
            calls.emplace_back(name);
            return otherOperation == name ? otherResult : sl::Result::eOk;
        }
    };
    Runtime* runtime{};
    sl::Result Options(const sl::ViewportHandle&, const sl::DLSSGOptions& options)
    {
        runtime->Call("options");
        ++runtime->optionsCalls;
        // The inspected NVIDIA 2.14.1 wrapper copies options before its budget warning.
        if (runtime->optionsResult == sl::Result::eOk || runtime->optionsResult == sl::Result::eWarnOutOfVRAM) {
            runtime->submitted = options;
        }
        return runtime->optionsResult;
    }
    sl::Result State(const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions* options)
    {
        Require(!options, "state query remains read-only without expensive VRAM estimate");
        runtime->Call("state");
        state = runtime->state;
        return runtime->stateResult;
    }
    bool Wait(void* context, void* fence, std::uint64_t value)
    {
        Require(context == runtime && fence == runtime->state.inputsProcessingCompletionFence &&
            value == runtime->state.lastPresentInputsProcessingCompletionFenceValue,
            "forward current input-reader fence and value during warnings");
        runtime->Call("wait");
        ++runtime->waits;
        return runtime->waitOK;
    }
    sl::Result NewToken(sl::FrameToken*& token, const std::uint32_t* index)
    {
        Require(!runtime->calls.empty() && runtime->calls.back() == "wait", "wait before acquiring next frame token");
        runtime->token.index = *index;
        token = &runtime->token;
        return runtime->Call("token");
    }
    sl::Result Constants(const sl::Constants& constants, const sl::FrameToken&, const sl::ViewportHandle&)
    {
        runtime->lastReset = constants.reset == sl::eTrue;
        runtime->lastDepthInverted = constants.depthInverted == sl::eTrue;
        return runtime->Call("constants");
    }
    sl::Result Tags(const sl::ViewportHandle&, const sl::ResourceTag*, std::uint32_t, sl::CommandBuffer*)
    { return runtime->Call("tags"); }
    sl::Result Reflex(const sl::ReflexOptions&) { return runtime->Call("reflex"); }
    sl::Result Sleep(const sl::FrameToken&) { return runtime->Call("sleep"); }
    sl::Result Marker(sl::PCLMarker marker, const sl::FrameToken&)
    { return runtime->Call(marker == sl::PCLMarker::ePresentStart ? "present-start" : "marker"); }
    SessionAPI API(Runtime& value)
    {
        runtime = &value;
        value.state.numFramesToGenerateMax = 5;
        value.state.bIsDynamicMFGSupported = sl::eTrue;
        value.state.minWidthOrHeight = 100;
        value.state.inputsProcessingCompletionFence = reinterpret_cast<void*>(0x1000);
        SessionAPI api{};
        api.setOptions = Options; api.getState = State; api.waitForInputReaders = Wait;
        api.newFrameToken = NewToken; api.setConstants = Constants; api.setTag = Tags;
        api.setReflexOptions = Reflex; api.reflexSleep = Sleep; api.marker = Marker; api.context = &value;
        return api;
    }
    bool TryPrepare(Session& session, bool depthInverted = false)
    {
        sl::Constants c{};
        for (auto* m : {&c.cameraViewToClip, &c.clipToCameraView, &c.clipToPrevClip, &c.prevClipToClip}) {
            for (unsigned row = 0; row < 4; ++row) {
                m->row[row] = {row == 0 ? 1.f : 0.f, row == 1 ? 1.f : 0.f,
                    row == 2 ? 1.f : 0.f, row == 3 ? 1.f : 0.f};
            }
        }
        c.cameraPos = {0, 0, 0}; c.cameraUp = {0, 1, 0}; c.cameraRight = {1, 0, 0}; c.cameraFwd = {0, 0, 1};
        c.jitterOffset = {0, 0}; c.mvecScale = {1, 1};
        c.cameraNear = .1f; c.cameraFar = 1000; c.cameraFOV = 1; c.cameraAspectRatio = 2;
        c.depthInverted = depthInverted ? sl::eTrue : sl::eFalse; c.cameraMotionIncluded = sl::eTrue;
        c.motionVectors3D = sl::eFalse; c.reset = sl::eFalse;
        FrameGuides guides{};
        guides.displayWidth = 2560; guides.displayHeight = 1440;
        for (auto* texture : {&guides.motion, &guides.depth}) {
            texture->resource = sl::Resource(sl::ResourceType::eTex2d, texture, 0);
            texture->resource.width = 2560; texture->resource.height = 1440;
            texture->extent = {0, 0, 2560, 1440};
        }
        return session.Prepare(c, guides, reinterpret_cast<sl::CommandBuffer*>(0x2000)) && session.CompleteInputWrites();
    }
    void Prepare(Session& session)
    {
        Require(TryPrepare(session), "prepare valid world and completed input writes");
    }
    void Frame(Session& session, unsigned outputs, bool enabled = true)
    {
        Prepare(session);
        Require(session.BeforePresent(enabled), "options budget warning must not stop native Present");
        runtime->Call("native-present");
        runtime->state.numFramesActuallyPresented = outputs;
        ++runtime->state.lastPresentInputsProcessingCompletionFenceValue;
        Require(session.AfterPresent(true), "continue through warning state and retirement");
    }
}

int main()
{
    {
        Runtime r; Session s;
        Require(s.Start(API(r), 7), "depth convention scenario starts");
        for (bool inverted : {false, true, false}) {
            Require(TryPrepare(s, inverted), "both depth conventions are valid session inputs");
            Require(r.lastDepthInverted == inverted, "producer depth convention reaches Streamline unchanged");
            Require(s.BeforePresent(true) && s.AfterPresent(true), "depth convention frame completes");
        }
    }
    // Exact reported sequence: healthy x4, state warning, then options warning.
    for (bool withStateWarning : {false, true}) {
        Runtime r; Session s;
        Require(s.Start(API(r), 7), "start healthy session");
        s.SetMFGUnlockState(true, true);
        Require(s.RequestGeneration({3, 0, false}), "request x4");
        r.stateResult = withStateWarning ? sl::Result::eWarnOutOfVRAM : sl::Result::eOk;
        Frame(s, 4);
        r.optionsResult = sl::Result::eWarnOutOfVRAM;
        const auto epoch = s.Snapshot().presentationEpoch;
        for (unsigned i = 0; i < 4; ++i) {
            Frame(s, 4);
            Require(!r.lastReset && s.Snapshot().GenerationActive(), "warning preserves temporal history and active x4");
            Require(r.submitted.numFramesToGenerate == 3 && r.submitted.mode == sl::DLSSGMode::eOn,
                "warning does not downgrade submitted multiplier");
        }
        Require(s.Snapshot().runtimePresentedFrames == 20 && s.OutputBatches().generatedOutputs == 15,
            "warning does not lose or double count outputs");
        Require(r.waits == 6 && s.Snapshot().presentationEpoch == epoch, "ordinary waits continue without recovery epoch");
        Require(s.Snapshot().optionsWarnings == 4 && s.Snapshot().optionsResult == sl::Result::eWarnOutOfVRAM,
            "count options warnings separately from state warnings");
        Require(s.RequestGeneration({1, 0, false}), "request x2 while over budget");
        Frame(s, 2);
        Require(r.submitted.numFramesToGenerate == 1, "changed multiplier is submitted during warning");
        Frame(s, 1, false);
        Require(r.submitted.mode == sl::DLSSGMode::eOff, "explicit Off honored during warning");
        Require(s.RequestGeneration({3, 120, true}), "request dynamic mode during warning");
        Frame(s, 3);
        Require(r.submitted.mode == sl::DLSSGMode::eDynamic && r.submitted.dynamicTargetFrameRate == 120,
            "dynamic request honored during warning");
        r.optionsResult = r.stateResult = sl::Result::eOk;
        Frame(s, 3);
        Require(s.Snapshot().failure == SessionFailure::None && !r.lastReset, "normal success recovers without resetting history");
        Require(s.Snapshot().optionsResult == sl::Result::eOk && s.Snapshot().optionsWarnings == 7,
            "successful submission clears active warning without discarding cumulative count");
    }
    // Startup, no-world pass-through, and resize also use the same options boundary.
    {
        Runtime r; Session s;
        r.optionsResult = r.stateResult = sl::Result::eWarnOutOfVRAM;
        Require(s.Start(API(r), 7), "startup options warning accepted");
        Require(s.BeforePresent(false) && r.optionsCalls == 1, "identical startup options are not retried");
        r.state.numFramesActuallyPresented = 1;
        Require(s.AfterPresent(true), "pass-through warning frame completes");
        s.SetMFGUnlockState(true, true);
        s.RequestGeneration({3, 0, false});
        Frame(s, 4);
        const auto frame = s.Snapshot().frameIndex;
        const auto outputs = s.Snapshot().runtimePresentedFrames;
        Require(s.Stop() && r.submitted.mode == sl::DLSSGMode::eOff, "warning permits disabling before retirement");
        Require(s.ResumeAfterResize() && s.Snapshot().frameIndex == frame + 1 &&
            s.Snapshot().runtimePresentedFrames == outputs, "resize preserves tokens without double-counting");
        Frame(s, 4);
        Require(r.lastReset, "actual resize still resets temporal history");
    }
    for (bool atStartup : {false, true}) {
        Runtime r; Session s;
        if (atStartup) { r.optionsResult = sl::Result::eErrorInvalidParameter; }
        const bool started = s.Start(API(r), 7);
        if (!atStartup) {
            Require(started, "error scenario starts");
            r.optionsResult = sl::Result::eErrorInvalidParameter;
            Prepare(s);
            Require(!s.BeforePresent(true), "real options failure stops Present");
        } else { Require(!started, "real startup options failure retained"); }
        Require(s.Snapshot().stage == SessionStage::Faulted && s.Snapshot().result == sl::Result::eErrorInvalidParameter,
            "real error result retained");
    }
    for (const char* operation : {"reflex", "constants", "tags", "present-start"}) {
        Runtime r; Session s; Require(s.Start(API(r), 7), "unrelated API scenario starts");
        Prepare(s);
        r.otherOperation = operation; r.otherResult = sl::Result::eWarnOutOfVRAM;
        bool success;
        if (r.otherOperation == "constants" || r.otherOperation == "tags") {
            // Exercise the next frame's preparation without using the assertion helper.
            Require(s.BeforePresent(true) && s.AfterPresent(true), "finish current frame");
            success = TryPrepare(s);
        } else { success = s.BeforePresent(true); }
        Require(!success && s.Snapshot().stage == SessionStage::Faulted &&
            s.Snapshot().failure == SessionFailure::Streamline && s.Snapshot().result == sl::Result::eWarnOutOfVRAM,
            "unrelated warning is not globally accepted");
    }
    {
        Runtime r; Session s; Require(s.Start(API(r), 7), "query failure scenario starts");
        r.optionsResult = sl::Result::eWarnOutOfVRAM;
        Prepare(s); Require(s.BeforePresent(true), "options warning accepted before real query error");
        r.stateResult = sl::Result::eErrorInvalidParameter;
        const auto waits = r.waits;
        Require(!s.AfterPresent(true) && s.Snapshot().failure == SessionFailure::Streamline && r.waits == waits,
            "real state error remains fatal without using invalid fence output");
    }
    {
        Runtime r; Session s; Require(s.Start(API(r), 7), "retirement failure scenario starts");
        r.optionsResult = r.stateResult = sl::Result::eWarnOutOfVRAM;
        Prepare(s); Require(s.BeforePresent(true), "options warning accepted before failed wait");
        r.waitOK = false;
        Require(!s.AfterPresent(true) && s.Snapshot().failure == SessionFailure::InputRetirement,
            "failed input-reader wait remains fatal during warnings");
    }
    {
        Runtime r; Session s; Require(s.Start(API(r), 7), "native failure scenario starts");
        r.optionsResult = sl::Result::eWarnOutOfVRAM;
        Prepare(s); Require(s.BeforePresent(true), "warning accepted before native failure");
        Require(!s.AfterPresent(false) && s.Snapshot().failure == SessionFailure::Present,
            "real native Present failure remains fatal");
    }
    std::puts("PASS: DLSS-G options/state budget warnings, recovery, resize and failure boundaries");
}
