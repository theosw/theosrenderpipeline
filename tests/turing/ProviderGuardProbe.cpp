// Manual check of the real 310.9.1 NVAPI thunk and provider-local resolver.
// Initializes NVAPI and issues its null presence probe; no device or dispatch.
#include "../../extern/MFGAmpere/runtime.cpp"
#include <iostream>
using namespace trp::ampere;
static void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int wmain(int argc,wchar_t** argv){try {
    Check(argc==2,"ProviderGuardProbe <runtime directory>");
    auto& s=State();s.targetSm=75;
    Check(Load(std::filesystem::absolute(argv[1]),L"nvngx_dlssg.dll",s.provider,false),"provider identity");
    memory::Image image;Check(image.Open(s.provider),"provider image");
    // Exact call contract recovered from this qualified image, not a portable
    // runtime offset. The production guard discovers its IAT by name instead.
    constexpr std::array<std::uint8_t,7> queryId{0xb9,0x7d,0x67,0x1a,0xad,0xff,0xd0};
    constexpr std::array<std::uint8_t,8> prolog{0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74};
    Check(image.OwnCode(image.base+0x2960,0xe5) && memory::Equal(image.base+0x2960,prolog) && memory::Equal(image.base+0x29b7,queryId),"310.9.1 CuModule thunk contract");
    s.nvapi=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    s.query=reinterpret_cast<Query>(GetProcAddress(s.nvapi,"nvapi_QueryInterface"));Check(s.query!=nullptr,"NVAPI query");
    s.importSlots[2]=image.Import("GetProcAddress");
    Check(s.importSlots[2] && memory::Read(s.importSlots[2],s.resolvers[2]) && s.resolvers[2],"provider resolver");
    const Resolver replacement=Resolve<2>;
    s.imports.Add(s.importSlots[2],{reinterpret_cast<std::uint8_t*>(&s.resolvers[2]),sizeof(void*)},
        {reinterpret_cast<const std::uint8_t*>(&replacement),sizeof(void*)});
    Check(s.imports.Commit(),"publish provider resolver");
    const auto thunk=reinterpret_cast<CreateModule>(image.base+0x2960);
    const int result=thunk(nullptr,nullptr,0,nullptr);
    Check(s.createModule.load()!=nullptr && (s.mask.load()&(1u<<14)),"real vendor thunk traversed production guard");
    Check(s.moduleCalls==0 && !s.failed && result!=0,"null presence probe must not latch failure");
    Check(s.imports.Rollback(),"restore provider resolver");
    std::cout<<"PASS real provider thunk reached guard; probe status="<<result<<"; no GPU dispatch\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
