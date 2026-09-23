// Manual offline check of the production startup planner against a supplied DLL.
// Loads the DLLs but never initializes Streamline, NGX, CUDA or a graphics device.
#include "../../extern/MFGAmpere/runtime.cpp"
#include <fstream>
#include <iostream>

using namespace trp::ampere;
static void Check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
static void WritePtx(std::span<const std::uint8_t> bytes, unsigned target,
                     const std::filesystem::path& output) {
    auto containerPath=output; containerPath.replace_extension(".fatbin");
    std::ofstream containerFile(containerPath,std::ios::binary);
    containerFile.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    Check(static_cast<bool>(containerFile),"fatbin output write");
    std::vector<mfgunlock::fatbin::Entry> entries; std::size_t end{};
    Check(mfgunlock::fatbin::Parse(bytes,entries,end),"replacement container parse");
    unsigned found{};
    for (const auto& entry:entries) {
        if (entry.kind!=1 || entry.architecture!=target) continue;
        ++found;
        std::vector<unsigned char> ptx;
        if (entry.flags & 0x2000) {
            ptx.resize(entry.unpacked_bytes);
            Check(mfgunlock::fatbin::Lz4BlockDecompress(bytes.data()+entry.PayloadOffset(),entry.compressed_bytes,
                ptx.data(),ptx.size()),"replacement PTX decode");
        } else ptx.assign(bytes.begin()+entry.PayloadOffset(),bytes.begin()+entry.End());
        const auto textEnd=std::find(ptx.begin(),ptx.end(),0);
        std::ofstream file(output,std::ios::binary);
        Check(static_cast<bool>(file),"PTX output open");
        file.write(reinterpret_cast<const char*>(ptx.data()),textEnd-ptx.begin());
        Check(static_cast<bool>(file),"PTX output write");
    }
    Check(found==1,"exactly one selected PTX program");
}
struct Container {
    std::uint8_t* address;
    std::vector<std::uint8_t> before, after;
};
int wmain(int argc,wchar_t** argv) { try {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
    Check(argc==4,"ProviderAudit <runtime-directory> <output-directory> <75|86>");
    const auto directory=std::filesystem::absolute(argv[1]), output=std::filesystem::absolute(argv[2]);
    const auto target=std::stoul(argv[3]); Check(target==75 || target==86,"target");
    std::filesystem::create_directories(output);
    auto& s=State(); s.targetSm=target;s.nativeArchitecture=target==75?kTuring:kAmpere;
    s.log=[](const char* message){ std::cout<<message<<'\n'; };
    Check(Load(directory,L"nvngx_dlssg.dll",s.provider,false),"load provider");
    Check(Load(directory,L"sl.dlss_g.dll",s.wrapper,false),"load wrapper");
    memory::Image image;Check(image.Open(s.provider),"provider image");
    std::vector<Container> containers;
    for (const auto& section:image.sections) {
        if (!(section.Characteristics&IMAGE_SCN_MEM_READ) || (section.Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
        auto* begin=image.base+section.VirtualAddress;const std::size_t size=section.Misc.VirtualSize;
        for (std::size_t p=0;p+16<=size;) {
            std::array<std::uint8_t,16> header{};Check(memory::Copy(header.data(),begin+p,16),"read header");
            if (fatbin::ReadU32(header.data())!=fatbin::kMagic) {++p;continue;}
            const auto payload=fatbin::ReadU64(header.data()+8);
            Check(payload<=size-p-16 && payload<=fatbin::kMaxFatbinBytes-16,"container bounds");
            const auto bytes=16+static_cast<std::size_t>(payload);
            std::vector<std::uint8_t> original(bytes);Check(memory::Copy(original.data(),begin+p,bytes),"read container");
            Plan plan;std::string reason;
            const auto result=RetargetForTarget(original.data(),bytes,plan,reason,target);
            if(result==Status::Rejected) throw std::runtime_error(reason);
            if(result==Status::Retargeted) {
                if(target==86) {
                    Plan previous;std::string oldReason;
                    Check(Retarget(original.data(),bytes,previous,oldReason)==Status::Retargeted &&
                        previous.replacement==plan.replacement,"existing SM86 bytes preserved");
                }
                WritePtx(plan.replacement,target,output/(std::to_string(containers.size())+".ptx"));
                containers.push_back({begin+p,std::move(original),std::move(plan.replacement)});
            }
            p+=bytes;
        }
    }
    Check(containers.size()==70,"exact provider qualification requires 70 programs");
    Check(PlanProvider(),"production provider plan");
    Check(s.fatbins==70,"production planner covers all programs");
    std::uintptr_t finalFatbin{};
    Check(memory::Read(reinterpret_cast<const void*>(s.temporal.replacementDescriptor+8),finalFatbin),"temporal descriptor");
    std::vector<std::uint8_t> temporal(s.temporal.outputBytes);
    Check(memory::Copy(temporal.data(),reinterpret_cast<const void*>(finalFatbin),temporal.size()),"temporal clone read");
    WritePtx(temporal,target,output/"temporal-final.ptx");
    Check(s.data.Commit(),"publish production transaction");
    for(const auto& c:containers)Check(memory::Equal(c.address,c.after),"published provider matches checked PTX");
    std::uintptr_t descriptor{};
    Check(memory::Read(reinterpret_cast<const void*>(s.temporal.slot),descriptor) && descriptor==s.temporal.replacementDescriptor,
        "temporal clone published");
    Check(s.minimumArch[1]==static_cast<std::uint8_t>(s.nativeArchitecture),"physical architecture gate");
    Check(!s.data.Commit(),"cannot publish transaction twice");
    Check(s.data.Rollback() && !s.data.Unsafe(),"production rollback");
    for(const auto& c:containers)Check(memory::Equal(c.address,c.before),"provider restored exactly");
    Check(memory::Read(reinterpret_cast<const void*>(s.temporal.slot),descriptor) && descriptor==s.temporal.originalDescriptor,
        "original temporal descriptor restored");
    Check(s.minimumArch[1]==0x90,"original architecture gate restored");
    std::cout<<"PASS target=SM"<<target<<" programs="<<containers.size()<<" temporal=1 transactionWrites="<<s.data.Count()
        <<" publish=verified rollback=verified no GPU dispatch\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n';return 1; } }
