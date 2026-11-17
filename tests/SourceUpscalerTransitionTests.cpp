#include "NvidiaUpscalerConfiguration.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace TheosRenderPipeline;
static void Require(bool value,const char* why)
{ if(!value) {std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);} }
static Upscaler::Configuration Ready(int mode=0)
{
    Upscaler::Configuration c;c.Initialize({mode,2,11,true,true});c.BeginSubmission();c.Completed(true);return c;
}
int main()
{
    auto c=Ready();
    for(unsigned i=0;i<20;++i) {
        const int target=i%2?0:4;
        c.Request({target,2,11,true,true});
        Require(!c.NeedsRestart() && c.NeedsLiveChange(),"warm algorithm change is live");
        std::vector<int> events;
        Require(Upscaler::ApplyLive(c,[&]{events.push_back(1);return true;},
            [&](const auto& request){Require(request.mode==target&&request.quality==2,"fixed allocation candidate");Require(c.Effective().mode!=target,"creation has not published effective mode");events.push_back(2);return true;},
            [&]{Require(c.Effective().mode!=target,"resume has not published effective mode");events.push_back(3);return true;}),"transition succeeds");
        Require(c.Effective().mode==target && events==std::vector<int>{1,2,3},"publish only after retirement creation and resume");
    }
    for(unsigned failure=1;failure<=3;++failure) {
        auto failed=Ready();failed.Request({4,2,11,true,true});std::vector<unsigned> events;
        Require(!Upscaler::ApplyLive(failed,[&]{events.push_back(1);return failure!=1;},
            [&](const auto&){events.push_back(2);return failure!=2;},[&]{events.push_back(3);return failure!=3;}),"each failed stage stops transition");
        Require(failed.Failed() && failed.Effective().mode==0 && events.size()==failure,"failure never publishes requested owner or reaches later stages");
    }
    auto pendingNative=Ready();pendingNative.Request({4,2,11,true,true});
    Upscaler::ApplyLive(pendingNative,[]{return true;},[](const auto&){return true;},[]{return true;});
    pendingNative.Request({3,2,11,true,true});
    Require(pendingNative.NeedsRestart() && pendingNative.LiveCandidate().mode==4 && !pendingNative.NeedsLiveChange(),
        "staging DLAA keeps the current live XeSS method until restart");
    auto refused=Ready();refused.Request({4,2,11,true,true});refused.RejectLiveMode();
    Require(refused.Ready() && !refused.Failed() && refused.NeedsRestart() && !refused.NeedsLiveChange(),"extent refusal retains working method without retry each frame");
    refused.Saved();Require(refused.Persisted().mode==4 && refused.Effective().mode==0,"refused saved mode remains separate from current method");
    refused.Request({0,2,11,true,true});refused.Request({4,2,11,true,true});
    Require(refused.NeedsLiveChange(),"explicit new mode selection can retry preflight");
    auto cold=Ready(4);cold.Request({0,2,11,true,true});
    Require(cold.NeedsRestart() && !cold.NeedsLiveChange(),"cold DLSS entry from XeSS startup remains guarded");
    cold.SetNvidiaServicesReady(true);
    Require(!cold.NeedsRestart() && cold.NeedsLiveChange(),"initialized host services admit fixed-size XeSS-started return");
    cold.RejectLiveMode();
    Require(cold.NeedsRestart() && !cold.NeedsLiveChange() && cold.Effective().mode==4,"DLSS range refusal retains cold XeSS owner");
    auto readyCold=Ready(4);readyCold.SetNvidiaServicesReady(true);readyCold.Request({0,2,11,true,true});
    Require(Upscaler::ApplyLive(readyCold,[]{return true;},[](const auto&){return true;},[]{return true;}),"admitted cold return follows full retirement/create/resume contract");
    readyCold.Request({4,2,11,true,true});Require(readyCold.NeedsLiveChange(),"XeSS-started session also returns to its original method");
    auto native=Ready(3);native.Request({4,2,11,true,true});
    Require(native.NeedsRestart() && native.LiveCandidate().mode==3,"native DLAA allocation remains restart-bound");
    auto quality=Ready();quality.Request({4,0,11,true,true});
    Require(quality.NeedsRestart() && quality.LiveCandidate().mode==4 && quality.LiveCandidate().quality==2,"mode can change without applying a pending render-size change");
    // FSR 4 (6) started sessions switch on Apply like FSR 3.1 (5); a sharpening-only
    // change in FSR 4 is a live change that keeps the mode.
    auto fsr4=Ready(6);fsr4.Request({5,2,11,true,true});
    Require(!fsr4.NeedsRestart() && fsr4.NeedsLiveChange() && fsr4.LiveCandidate().mode==5,"FSR 4 session switches to FSR 3.1 live");
    auto fsr31=Ready(5);fsr31.Request({6,2,11,true,true});
    Require(fsr31.NeedsLiveChange() && fsr31.LiveCandidate().mode==6,"FSR 3.1 session switches to FSR 4 live");
    auto sharpen=Ready(6);sharpen.Request({6,2,11,false,true});
    Require(sharpen.NeedsLiveChange() && sharpen.LiveCandidate().mode==6,"FSR 4 setting change keeps FSR 4");
    std::puts("PASS repeated warm switches, stage failures, refusal, persistence and cold/native guards, FSR 3.1/4 switching");
}
