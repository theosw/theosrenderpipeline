#include "XeFGUnlock.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace XeFGExperiment;
static unsigned checks{};
static void Require(bool condition,const char* name) {
    ++checks;if(!condition) {std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}
}
struct Fake final:Memory {
    std::vector<std::uint8_t> bytes=std::vector<std::uint8_t>(0x300000,0xcc);
    std::map<std::uint32_t,DWORD> protection;
    unsigned writes{},protects{},flushes{},reads{};
    unsigned failWrite{},failProtect{},failFlush{},corruptWrite{};
    bool failRollback{},mutateAtProtect{},injected{};
    explicit Fake(const std::vector<Patch>& plan) {
        for(const auto& p:plan) {
            std::copy(p.original.begin(),p.original.end(),bytes.begin()+p.rva);
            protection[p.rva&~4095u]=PAGE_EXECUTE_READ;
        }
    }
    bool Read(std::uint32_t rva,std::span<std::uint8_t> out) override {
        ++reads;if(rva>bytes.size()||out.size()>bytes.size()-rva) return false;
        std::copy_n(bytes.begin()+rva,out.size(),out.begin());return true;
    }
    bool Write(std::uint32_t rva,std::span<const std::uint8_t> input) override {
        ++writes;
        if(protection[rva&~4095u]!=PAGE_EXECUTE_READWRITE) return false;
        if(writes==failWrite || (injected&&failRollback)) {
            bytes[rva]=input[0];injected=true;return false; // A failed write may have partially written.
        }
        std::copy(input.begin(),input.end(),bytes.begin()+rva);
        if(writes==corruptWrite) {bytes[rva]^=1;injected=true;}
        return true;
    }
    bool Protect(std::uint32_t page,DWORD desired,DWORD& old) override {
        ++protects;old=protection[page];if(protects==failProtect) return false;
        protection[page]=desired;
        if(mutateAtProtect) {bytes[0x20da4f]^=1;mutateAtProtect=false;}
        return true;
    }
    bool ProtectionIs(std::uint32_t page,DWORD expected) override {return protection[page]==expected;}
    bool Flush() override {return ++flushes!=failFlush;}
};
int main() {
    const auto plan=MakePlan(0x1234567812345678,0x2234567812345678,0x3234567812345678);
    Require(plan.size()==8,"five gates and three required pacing hooks");
    std::string reason;
    Fake ok(plan);const auto before=ok.bytes;const auto protections=ok.protection;
    Require(Publish(ok,plan,static_cast<std::uint32_t>(ok.bytes.size()),reason)==PatchResult::Applied,"complete publication");
    Require(ok.protection==protections,"all page protection restored");
    Require(ok.flushes==1,"single successful cache flush");
    Require(Publish(ok,plan,static_cast<std::uint32_t>(ok.bytes.size()),reason)==PatchResult::Refused,"repeat does not patch an already published image");
    for(unsigned n=1;n<=plan.size();++n) {
        Fake f(plan);f.failWrite=n;
        Require(Publish(f,plan,static_cast<std::uint32_t>(f.bytes.size()),reason)==PatchResult::Refused,"each write failure safely refused");
        Require(f.bytes==before && f.protection==protections,"each partly written current site restored");
    }
    for(unsigned n=1;n<=protections.size()*2;++n) {
        Fake f(plan);f.failProtect=n;
        Require(Publish(f,plan,static_cast<std::uint32_t>(f.bytes.size()),reason)==PatchResult::Refused,"each acquire/restore failure safely refused");
        Require(f.bytes==before && f.protection==protections,"failed protection leaves original image");
    }
    Fake flush(plan);flush.failFlush=1;
    Require(Publish(flush,plan,static_cast<std::uint32_t>(flush.bytes.size()),reason)==PatchResult::Refused,"cache flush failure rolls back");
    Require(flush.bytes==before && flush.protection==protections,"flush rollback verified");
    Fake corrupt(plan);corrupt.corruptWrite=3;
    Require(Publish(corrupt,plan,static_cast<std::uint32_t>(corrupt.bytes.size()),reason)==PatchResult::Refused,"write-readback mismatch rolls back");
    Require(corrupt.bytes==before,"corrupt current site restored");
    Fake unsafe(plan);unsafe.failWrite=2;unsafe.failRollback=true;
    Require(Publish(unsafe,plan,static_cast<std::uint32_t>(unsafe.bytes.size()),reason)==PatchResult::Unsafe,"rollback failure cannot admit x2");
    Fake changed(plan);changed.bytes[plan[6].rva]^=1;const auto foreign=changed.bytes;
    Require(Publish(changed,plan,static_cast<std::uint32_t>(changed.bytes.size()),reason)==PatchResult::Refused,"changed pacing bytes reject whole unlock");
    Require(changed.bytes==foreign && changed.writes==0 && changed.protects==0,"foreign hook untouched");
    Fake raced(plan);raced.mutateAtProtect=true;
    Require(Publish(raced,plan,static_cast<std::uint32_t>(raced.bytes.size()),reason)==PatchResult::Unsafe,"concurrent byte change classified unsafe");
    Require(raced.writes==0,"concurrent owner's change not overwritten");
    auto invalid=plan;invalid[0].rva=0x2fffff;Fake bounds(plan);
    Require(Publish(bounds,invalid,static_cast<std::uint32_t>(bounds.bytes.size()),reason)==PatchResult::Refused,"out-of-image write refused");
    auto overlap=plan;overlap.push_back(plan[0]);Fake overlaps(plan);
    Require(Publish(overlaps,overlap,static_cast<std::uint32_t>(overlaps.bytes.size()),reason)==PatchResult::Refused,"overlapping patches refused");
    std::printf("PASS checks=%u transaction failure and rollback coverage; no SDK execution\n",checks);
}
