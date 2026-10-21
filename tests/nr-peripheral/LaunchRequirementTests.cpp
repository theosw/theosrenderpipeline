#define TRP_ENABLE_PASCAL_RESEARCH 1
#include "FrameGen/NeuralResearch.h"
#include "../../src/FrameGen/NeuralLaunchHook.cpp"
#include <cstdio>
#include <cstdlib>
#include <array>

using namespace TheosRenderPipeline::NeuralRendering;
void Require(bool ok, const char* message) { if (!ok) { std::printf("FAIL %s\n",message); std::exit(1); } }
namespace TheosRenderPipeline::NeuralRendering { int RuntimeKernelBundle(HMODULE, const void*, std::uint32_t) { return 0; } }
int __cdecl RejectEmpty(ID3D12GraphicsCommandList*, const KernelLaunch*, std::uint32_t count) { Require(count==0,"empty probe"); return -5; }
int __cdecl DestroyOK(ID3D12Device*,void*) { return 0; }
unsigned calls{};
int __cdecl Forward(ID3D12GraphicsCommandList*, const KernelLaunchEx* launches, std::uint32_t count) {
    ++calls; Require(count==3 && launches[1].block.x==128 && launches[1].kernelParams, "real hook forwarded whole Ex chain"); return 0;
}
int main()
{
    LaunchBindings b;
    b.requirements["native"]={0,3,3,{128,1,1},{7,2,1},4096};
    auto* a=reinterpret_cast<void*>(1); auto* c=reinterpret_cast<void*>(2);
    int params{}; void* kernelParams[]{&params};
    b.names[a]="native"; b.names[c]="helper";
    Require(b.Ready(),"native requirement bound");
    std::array<KernelLaunchEx,3> input{{{c,{3,1,1},{32,1,1},0,&params,4,kernelParams},
        {a,{8,4,1},{32,1,1},128,&params,4,kernelParams},{c,{3,1,1},{32,1,1},0,&params,4,kernelParams}}};
    std::vector<KernelLaunchEx> output;
    Require(b.Rewrite(input.data(),3,output),"Ex accepted");
    Require(output.size()==3 && output[0].function==c && output[1].function==a && output[2].function==c,"chain whole and ordered");
    Require(output[1].block==LaunchDim{128,1,1} && output[1].grid==LaunchDim{7,2,1} && output[1].sharedBytes==4096,"geometry applied");
    Require(output[1].params==&params && output[1].paramSize==4 && output[1].kernelParams==kernelParams,"Ex parameter pointers preserved");
    Require(input[1].block.x==32 && output[0].block.x==32,"caller and helpers unchanged");
    KernelLaunch plain{a,{8,4,1},{32,1,1},128,&params,4};
    std::vector<KernelLaunch> rewritten;
    Require(b.Rewrite(&plain,1,rewritten) && rewritten[0].block.x==128,"plain covered");
    Active().bindings=b;
    Active().chain=&RejectEmpty;
    Require(LaunchChain(nullptr,nullptr,0)==-5 && Active().bindings.failure.empty(),"failed empty NVAPI capability probe does not poison admission");
    Active().chainEx=&Forward;
    Require(LaunchChainEx(nullptr,input.data(),3)==0 && calls==1,"real Ex hook forwarding");
    Active().bindings.names.erase(a);
    Require(LaunchChainEx(nullptr,input.data(),3)!=0 && calls==1,"unknown handle does not reach driver");
    Active().bindings.failure.clear();
    Active().bundles[c]=1;
    void* handle{};
    Require(CreateFunction(nullptr,c,"native",&handle)!=0,"required kernel in wrong bundle refused before driver");
    Active().bindings=b; Active().destroyModule=&DestroyOK; Active().modules[a]=c;
    Require(DestroyModule(nullptr,c)==0 && !Active().bindings.names.contains(a),"module destruction clears bindings");
    b.names.erase(a);
    Require(!b.Ready() && !b.Rewrite(&plain,1,rewritten),"destroyed/reused handle refused");
    b.failure.clear(); b.names[a]="helper";
    Require(!b.Ready(),"unknown required kernel refuses admission");
    b.names[a]="native"; b.requirements["native"].flags=0;
    Require(b.Rewrite(&plain,1,rewritten) && rewritten[0].grid==plain.grid && rewritten[0].sharedBytes==128,"optional fields preserved");

    std::vector<std::uint8_t> metadata{'T','R','P','L','C','H','1',0};
    auto word=[&](std::uint32_t x) { const auto* p=reinterpret_cast<const std::uint8_t*>(&x); metadata.insert(metadata.end(),p,p+4); };
    for (const auto x:{1u,1u,0u,3u,6u,0u,128u,1u,1u,0u,0u,0u,0u}) { word(x); }
    for (auto c1:std::string("native")) { metadata.push_back(static_cast<std::uint8_t>(c1)); }
    metadata.resize(68);
    LaunchRequirements rules; std::string reason;
    Require(ReadLaunchRequirements(metadata,rules,reason) && rules.at("native").block.x==128,"v2 decoded");
    for (std::size_t n=0;n<metadata.size();++n) { Require(!ReadLaunchRequirements(std::span(metadata).first(n),rules,reason),"truncated metadata refused"); }
    const auto temp=std::filesystem::temp_directory_path()/("trp-pascal-config-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    std::filesystem::create_directory(temp);
    const auto ini=temp/L"diagnostics.ini";
    wchar_t savedMode[32768]{},savedPack[32768]{};
    GetEnvironmentVariableW(L"TRP_NR_PASCAL_RESEARCH",savedMode,32768);
    GetEnvironmentVariableW(L"TRP_NR_PASCAL_PACK",savedPack,32768);
    SetEnvironmentVariableW(L"TRP_NR_PASCAL_RESEARCH",nullptr); SetEnvironmentVariableW(L"TRP_NR_PASCAL_PACK",nullptr);
    Require(!ReadPascalConfiguration(ini,temp).mode,"research defaults off");
    WritePrivateProfileStringW(L"NeuralRenderingResearch",L"EnablePascal",L"1",ini.c_str());
    WritePrivateProfileStringW(L"NeuralRenderingResearch",L"PascalPack",L"NVIDIA/c24.trpk",ini.c_str());
    auto config=ReadPascalConfiguration(ini,temp);
    Require(config.mode==L'1' && config.pack==temp/L"NVIDIA/c24.trpk","INI enables research and resolves relative pack");
    SetEnvironmentVariableW(L"TRP_NR_PASCAL_RESEARCH",L"0");
    SetEnvironmentVariableW(L"TRP_NR_PASCAL_PACK",L"C:/absolute/c24.trpk");
    config=ReadPascalConfiguration(ini,temp);
    Require(!config.mode && config.pack==std::filesystem::path(L"C:/absolute/c24.trpk"),"environment overrides INI including off and absolute pack");
    SetEnvironmentVariableW(L"TRP_NR_PASCAL_RESEARCH",savedMode[0]?savedMode:nullptr);
    SetEnvironmentVariableW(L"TRP_NR_PASCAL_PACK",savedPack[0]?savedPack:nullptr);
    std::filesystem::remove(ini); std::filesystem::remove(temp);
    metadata.push_back(0);
    Require(!ReadLaunchRequirements(metadata,rules,reason),"trailing bytes refused");
    metadata.pop_back(); metadata[32]=0; metadata[33]=8;
    Require(!ReadLaunchRequirements(metadata,rules,reason),"oversized block refused");
}
