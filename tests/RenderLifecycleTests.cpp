#include "FrameGen/StartupFallback.h"
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
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
    std::puts("PASS: bounded startup recovery policy");
}
