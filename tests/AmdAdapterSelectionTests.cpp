#include "FrameGen/AmdAdapterSelection.h"
#include "../third_party/amd-nr/trp_adapter_selection.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace TheosRenderPipeline::AMDAdapter;
void Require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
int main() {
    try {
        static_assert(offsetof(trp_dlss5nr_create_info,base)==0);
        static_assert(offsetof(trp_dlss5nr_create_info,adapter_pci_bus_id)==sizeof(dlss5nr_create_info));
        Require(sizeof(trp_dlss5nr_create_info)>sizeof(dlss5nr_create_info),"adapter extension leaves original C ABI prefix intact");
        PCI pci{};
        Require(ParsePCI("0000:2a:00.0",pci) && pci.bus==42,"hex PCI bus parses");
        for(auto bad:{"0000:2a:00.0oops","0000:2a:00",":2a:00.0","0000:ff:20.0","0000:00:00.8","garbage"})
            Require(!ParsePCI(bad,pci),"invalid PCI fails closed");
        PCI target{0,42,0,0};
        std::vector<Identity> devices{{0,11,{0,1,0,0},true,true},{1,99,target,true,true}};
        auto s=Select(devices.data(),devices.size(),99,&target);
        Require(s.result==Result::Selected && s.ordinal==1 && s.byLUID,"dGPU ordinal 1 selected over iGPU 0");
        devices.push_back({2,0,target,false,true});
        Require(Select(devices.data(),devices.size(),99,&target).ordinal==1,"exact LUID takes priority over weaker PCI-only candidate");
        devices.pop_back();
        s=Select(devices.data(),devices.size(),99,nullptr);
        Require(s.result==Result::Selected && s.ordinal==1,"exact LUID works when Windows PCI query fails");
        devices[1].luidKnown=false;
        s=Select(devices.data(),devices.size(),99,&target);
        Require(s.result==Result::Selected && s.ordinal==1 && !s.byLUID,"PCI fallback when HIP LUID is unavailable");
        Require(Select(devices.data(),devices.size(),99,nullptr).result==Result::NoMatch,"query failure cannot select ordinal zero");
        devices.push_back(devices[1]);devices.back().ordinal=2;
        Require(Select(devices.data(),devices.size(),99,&target).result==Result::Ambiguous,"ambiguous PCI fallback rejected");
        devices.pop_back();
        devices[1].luidKnown=true; devices[1].luid=123;
        Require(Select(devices.data(),devices.size(),99,&target).result==Result::NoMatch,"contradictory LUID rejects even if PCI matches");
        devices[1].luid=99; devices.push_back(devices[1]); devices.back().ordinal=2;
        Require(Select(devices.data(),devices.size(),99,&target).result==Result::Ambiguous,"duplicate identity rejected");
        devices.pop_back(); std::swap(devices[0],devices[1]);devices[0].ordinal=0;devices[1].ordinal=1;
        Require(Select(devices.data(),devices.size(),99,&target).ordinal==0,"independent CUDA order still selects same hardware");
        Require(Select(nullptr,0,99,&target).result==Result::NoMatch,"empty enumeration rejected");
        puts("PASS: HIP/CUDA ordinal independence, LUID identity, PCI fallback, query failure, contradiction and ambiguity. No HIP execution.");
    } catch(const std::exception& e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
