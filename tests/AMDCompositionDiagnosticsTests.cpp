#include "../third_party/amd-nr/runtime/core/composition_diagnostics.h"
#include <cstdio>
#include <cstdlib>
using namespace dlss5nr::diagnostics;
static void Require(bool value, const char* why) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main() {
    CompositionTrace trace;
    unsigned details = 0, summaries = 0;
    // An intermittent copy must remain counted after detailed logging is capped.
    for (unsigned i=0; i<120; ++i) {
        Frame f; f.apply = i%3 != 0; f.fresh = i%3 == 1;
        f.reason = f.apply ? CopyReason::None : CopyReason::NoFlow;
        f.age = i%5; f.latency = 3; f.reset = i==61; f.discarded = i==62;
        auto out = trace.Observe(f); details += out.detail; summaries += out.summary;
        Require(out.summary == (i==119), "one summary at the precise window boundary");
        if (out.summary) {
            auto w=out.window;
            Require(w.frames==120 && w.fresh==40 && w.reuse==40 && w.copy==40, "every recorded frame classified");
            Require(w.reasons[static_cast<unsigned>(CopyReason::NoFlow)]==40, "copy reason preserved beyond log budget");
            Require(w.resets==1 && w.discarded==1 && w.ageMax==4 && w.latencyMax==3, "reset/discard/age evidence retained");
            Require(w.details==8 && w.suppressed==112, "rapid alternation cannot flood detail logs");
        }
    }
    Require(details==8 && summaries==1 && trace.Pending().frames==0, "window reset retains no stale counts");
    Frame reuse; reuse.apply=true; reuse.wait=true;
    auto first=trace.Observe(reuse);
    Require(first.detail && first.window.reuse==1, "next window restores detail budget");
    reuse.wait=false; Require(trace.Observe(reuse).detail, "live execution switch is observed");
    reuse.tracking=true; Require(trace.Observe(reuse).detail, "live tracking switch is observed");
    auto stable=trace.Observe(reuse); Require(!stable.detail && stable.window.reuse==4, "stable reuse counted without repeated logging");
    for (auto reason : {CopyReason::NotRunning,CopyReason::NoResidual,CopyReason::NoFlow,CopyReason::Expired,CopyReason::DetailZero}) {
        Frame copy; copy.reason=reason; trace.Observe(copy);
        Require(trace.Pending().reasons[static_cast<unsigned>(reason)]>=1, "all copy reasons available in partial windows");
        Require(Name(reason)[0]!='\0', "copy reason label");
    }
    Require(trace.Pending().frames==9, "partial window can be flushed at retirement");
    std::puts("AMD composition trace: complete accounting, bounded detail, live toggles and retirement passed");
}
