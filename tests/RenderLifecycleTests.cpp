#include "FrameGen/StartupFallback.h"
#include "ReShadeLifecycle.h"
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>
using namespace TheosRenderPipeline;
static void Require(bool value,const char* why) { if(!value){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);} }
int main() {
    using SourceDLSSG::CanUseStartupTransport;
    for(auto failure:{HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND),E_NOINTERFACE,E_FAIL,DXGI_ERROR_UNSUPPORTED})
        Require(CanUseStartupTransport(failure,S_OK,false),"missing runtime/export, init and unsupported-feature failure before presenter can fall back");
    for(auto failure:{E_UNEXPECTED,E_INVALIDARG,E_POINTER})
        Require(!CanUseStartupTransport(failure,S_OK,false),"ownership conflicts and invalid contracts remain terminal");
    Require(!CanUseStartupTransport(E_FAIL,DXGI_ERROR_DEVICE_REMOVED,false),"removed device cannot fall back");
    Require(!CanUseStartupTransport(DXGI_ERROR_DEVICE_REMOVED,S_OK,false),"device-loss result remains terminal even before the removal query catches up");
    Require(!CanUseStartupTransport(E_FAIL,S_OK,true),"presenter/FG session/first Present failures cannot create a second owner");
    Require(!CanUseStartupTransport(E_FAIL,S_OK,false,true),"unknown submitted work cannot fall back");
    ReShadeLifecycleTrace trace;
    trace.Record({}); Require(trace.Snapshot().total==0,"observer defaults off");
    trace.Enable(true);
    for(unsigned i=0;i<140;++i) {
        ReShadeLifecycleRecord event;event.runtime=i+10;event.owned=i%2==0; event.event=ReShadeLifecycleEvent::InitRuntime;
        trace.Record(event);
    }
    const auto snapshot=trace.Snapshot();
    Require(snapshot.count==128 && snapshot.total==140 && snapshot.overwritten==12,"bounded observer retains recent identities");
    Require(snapshot.records[0].sequence==13 && snapshot.records[127].sequence==140 && snapshot.records[0].runtime==22,"chronological identity ordering");
    trace.Enable(false); trace.Record({}); Require(trace.Snapshot().total==140,"disabled observer is inert");
    std::puts("PASS: bounded startup recovery policy and passive lifecycle observer");
}
