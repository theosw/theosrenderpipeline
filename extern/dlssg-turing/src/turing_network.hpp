#pragma once
#include "module_patch.hpp"
#include <array>

namespace trp::ampere::turing_network {
// NVIDIA DLSS-G 310.9.1, provider SHA-256 FF6E90EB78B827927DFF5B4ECC6B1C870
// C2E9BCA29ED9F48C7D348CC9E170B82. Other layouts are deliberately unsupported.
// The <89 implementation embeds raw SM86 cubins. The >89 implementation uses
// the PTX-bearing fatbins prepared by our SM75 lowering. Both share weights,
// object layout, dimension/dispatch methods and kernel launch contracts.
// Select that implementation only after binding a physical RTX20 and preparing
// every referenced container. NVAPI architecture reporting remains physical.
struct Code { std::uint32_t rva, bytes; std::uint64_t fingerprint; };
inline constexpr std::array<Code,6> kCode{{
    {0x4d9f0,0x381,0x87f956430958b6adull}, // EndpointDL4RTWrapper::CreateImpl
    {0x46410,0x14e0,0x1673fa7bd3d78b8eull}, // DL1 PTX constructor
    {0x4b730,0x1f1f,0x6110aa99dd5aee70ull}, // DL2 PTX constructor
    {0x5d1e0,0xc1a,0xe4486da426b72aafull}, // DL1 weights/kernel initialization
    {0x601a0,0x11c6,0x095d22d6fb2bd5dd7ull}, // DL2 weights/kernel initialization
    {0x1b6c0,0x76,0x9ff9056703bfdf13ull}, // physical architecture -> SM getter
}};
struct Program { std::uint32_t rva, bytes; };
inline constexpr std::array<Program,39> kPrograms{{
    {0x629800,0x4080},{0x62d880,0x7f0},{0x62e070,0x1358},
    {0x31cc40,0x2ba0},{0x633fb0,0x2c98},{0x636c50,0x39a0},
    {0x63a5f0,0x2c40},{0x62f3d0,0x4be0},{0x63d230,0x8f8},
    {0x63db30,0x868},{0x63e3a0,0x2c38},{0x1d04d0,0x2c40},
    {0x640fe0,0x8f8},{0x6418e0,0x868},{0x642150,0x1cc8},
    {0x643e20,0x20f8},{0x645f20,0x2130},{0x648050,0x8f8},
    {0x648950,0x868},{0x1fb100,0x17c8},{0x6491c0,0x1768},
    {0x4cc1f0,0x8f8},{0x5a2cc0,0x860},{0x5b7380,0x12e8},
    {0x5f5980,0xf48},{0x1c1c60,0x390},{0x1c1ff0,0x1060},
    {0x1c3050,0x1508},{0x1c4560,0x1478},{0x1c59e0,0x14e8},
    {0x1c6ed0,0x12e8},{0x18da20,0x1b40},{0x1c81c0,0x840},
    {0x1c8a00,0x1678},{0x1ca080,0x13e8},{0x1cb470,0x1060},
    {0x1cc4d0,0x1320},{0x1cd7f0,0x1490},{0x1cec80,0x1850},
}};
inline constexpr std::array<std::uint32_t,2> kSites{0x4da50,0x4db91};
inline constexpr std::array<std::array<std::uint8_t,2>,2> kBefore{{{0x7e,0x23},{0x7e,0x20}}};
inline constexpr std::array<std::uint8_t,2> kSelected{0x90,0x90};

template<class PreparedProgram>
bool Plan(const memory::Image& image,memory::Transaction& transaction,
          std::array<std::uint8_t*,2>& sites,PreparedProgram prepared) {
    // Qualify complete code bodies, not just a common compare/branch opcode.
    // These exact bodies contain no base relocations. Check their unwind extents
    // too; RVA plus bytes alone must not identify a partial/wrong function.
    for(const auto& code:kCode) {
        if(!image.Contains(code.rva,code.bytes))return false;
        auto* address=image.base+code.rva;
        if(!image.OwnCode(address,code.bytes))return false;
        DWORD64 base{};const auto* f=RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(address),&base,nullptr);
        if(!f || base!=reinterpret_cast<DWORD64>(image.base) ||
            f->BeginAddress!=code.rva || f->EndAddress!=code.rva+code.bytes)return false;
        std::vector<std::uint8_t> bytes(code.bytes);
        if(!memory::Copy(bytes.data(),address,bytes.size()))return false;
        std::uint64_t hash=14695981039346656037ull;
        for(const auto b:bytes){hash^=b;hash*=1099511628211ull;}
        if(hash!=code.fingerprint)return false;
    }
    for(const auto& program:kPrograms)
        if(!image.Contains(program.rva,program.bytes) || !prepared(image.base+program.rva,program.bytes))return false;
    for(std::size_t i=0;i<kSites.size();++i)
        if(!memory::Equal(image.base+kSites[i],kBefore[i]))return false;
    for(std::size_t i=0;i<kSites.size();++i) {
        sites[i]=image.base+kSites[i];
        transaction.Add(sites[i],kBefore[i],kSelected);
    }
    return true;
}
inline bool Selected(const std::array<std::uint8_t*,2>& sites) noexcept {
    for(const auto* site:sites)if(!site || !memory::Equal(site,kSelected))return false;
    return true;
}
} // namespace trp::ampere::turing_network
