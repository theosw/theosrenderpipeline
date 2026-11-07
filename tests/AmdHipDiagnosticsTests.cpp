#include "../third_party/amd-nr/trp_hip_diagnostics.h"
#include "../third_party/amd-nr/trp_diagnostics.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
void Require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
int main() {
    try {
        using namespace TRPAmdDiagnostics;
        auto absent=CurrentHip();
        Require(absent.deviceResult==-1 && absent.contextResult==-1 && !GetModuleHandleW(L"amdhip64_7.dll"),"observer must not load HIP");
        auto module=LoadLibraryW(L"amdhip64_7.dll"); Require(module!=nullptr,"CPU fake HIP fixture loads");
        auto select=reinterpret_cast<void (*)(int,int)>(GetProcAddress(module,"trp_test_select")); Require(select!=nullptr,"fixture control");
        auto discrete=CurrentHip(); Require(discrete.device==1 && discrete.luid==0x17573 && discrete.context==reinterpret_cast<void*>(0x1001) &&
            discrete.deviceResult==0 && discrete.luidResult==0 && !std::strcmp(discrete.pci,"0000:03:00.0"),"capture discrete identity/context");
        select(0,0);auto integrated=CurrentHip(); Require(integrated.device==0 && integrated.luid==0x19981 && integrated.context==reinterpret_cast<void*>(0x1000),"observe a changed context without correcting it");
        auto again=CurrentHip(); Require(again.device==integrated.device && again.context==integrated.context,"observer preserves selection");
        select(1,1);auto failed=CurrentHip(); Require(failed.contextResult==201 && failed.deviceResult==202 && failed.luidResult==-1 && !failed.luid,"query errors remain explicit unknown identity");
        char prefix[160]{};Prefix(prefix,sizeof(prefix));Require(std::strstr(prefix,Session()) && std::strstr(prefix,"Z " TRP_AMD_DIAGNOSTIC_LABEL " session=") && std::strstr(prefix," tid="),"timestamp, release label and process identity");
        Require(!GetProcAddress(module,"hipSetDevice") && !GetProcAddress(module,"hipCtxSetCurrent"),"fixture supplies no context mutation API");
        std::puts("PASS: CPU-only HIP observer; unloaded runtime, discrete/integrated contexts, query failures, no context mutation; stable session prefix.");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what());return 1; }
}
