#include "HookSafety.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace TheosRenderPipeline::HookSafety;
namespace
{
    void Require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
    using Function = int (*)();
    Function original{};
    int First() { return 40; }
    int Hook() { return original() + 2; }
    int Later() { return Hook() + 1; }
    int Foreign() { return 90; }
    int Invoke(std::uintptr_t address) { return reinterpret_cast<Function>(address)(); }
    using ClientRect = decltype(&GetClientRect);
    ClientRect earlierRect{}, originalRect{};
    BOOL WINAPI EarlierRect(HWND, LPRECT rect) { if (rect) { rect->right = 73; } return TRUE; }
    BOOL WINAPI OurRect(HWND window, LPRECT rect) { return originalRect(window, rect); }
    __declspec(noinline) BOOL CallRect(LPRECT rect) { return GetClientRect(nullptr, rect); }

    struct Page
    {
        void* data = VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS);
        Page() { if (data && !VirtualAlloc(data, 4096, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) { VirtualFree(data, 0, MEM_RELEASE); data = nullptr; } }
        ~Page() { if (data) { VirtualFree(data, 0, MEM_RELEASE); } }
        std::uintptr_t At(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(data) + offset; }
    };
    void CallAt(std::uintptr_t site, std::uintptr_t target)
    {
        const std::uint8_t opcode = 0xE8;
        const auto displacement = static_cast<std::int32_t>(target - site - 5);
        std::memcpy(reinterpret_cast<void*>(site), &opcode, 1);
        std::memcpy(reinterpret_cast<void*>(site + 1), &displacement, 4);
    }
}

int main()
{
    try {
        Page page;
        Require(page.data != nullptr, "allocate executable test memory");
        const auto site = page.At(32), callee = page.At(128), other = page.At(160), chain = page.At(1024);
        CallAt(site, callee);
        Require(DirectCall(site, callee, page.At(), 512), "accept verified direct call");
        Require(!DirectCall(site, other, page.At(), 512), "reject wrong engine callee despite E8 opcode");
        CallAt(site, chain);
        Require(DirectCall(site, callee, page.At(), 512), "preserve earlier executable trampoline");
        *reinterpret_cast<std::uint8_t*>(site) = 0x90;
        Require(!DirectCall(site, callee, page.At(), 512), "reject changed call instruction");
        Require(!DirectCall(1, callee, page.At(), 512), "unreadable call fails without exception");

        constexpr std::array<std::uint8_t, 10> expected{0x80,0x79,0x18,0,0x0F,0x84,0xC0,0,0,0};
        std::memcpy(reinterpret_cast<void*>(site), expected.data(), expected.size());
        Require(Bytes(site, expected), "verified full NOP replacement range accepted");
        *reinterpret_cast<std::uint8_t*>(site + 9) = 1;
        Require(!Bytes(site, expected), "mismatch at final byte of patch rejected");
        Require(!Bytes(page.At(4092), expected), "unreadable end of patch rejected");
        std::memcpy(reinterpret_cast<void*>(site), expected.data(), expected.size());
        Require(Entry(site, expected, page.At(), 512), "accept original entry instructions");
        CallAt(site, chain);
        *reinterpret_cast<std::uint8_t*>(site) = 0xE9;
        Require(Entry(site, expected, page.At(), 512), "preserve existing external entry detour");
        CallAt(site, other);
        *reinterpret_cast<std::uint8_t*>(site) = 0xE9;
        Require(!Entry(site, expected, page.At(), 512), "unexpected engine-internal entry jump rejected");
        const auto slotAddress = page.At(512);
        const std::uint8_t indirect[]{0xFF,0x15};
        const auto indirectDisplacement = static_cast<std::int32_t>(slotAddress - site - 6);
        std::memcpy(reinterpret_cast<void*>(site), indirect, 2);
        std::memcpy(reinterpret_cast<void*>(site + 2), &indirectDisplacement, 4);
        Require(ImportCallSlot(site) == slotAddress, "six-byte import resolves exact IAT slot");
        *reinterpret_cast<std::uint8_t*>(site + 1) = 0x25;
        Require(!ImportCallSlot(site), "jump is not an import call");

        auto* slot = reinterpret_cast<std::uintptr_t*>(page.At(512));
        *slot = reinterpret_cast<std::uintptr_t>(&First);
        SlotRegistry registry;
        Require(registry.Install(slot, reinterpret_cast<std::uintptr_t>(&Hook), original), "install first vtable hook");
        Require(Invoke(*slot) == 42, "hook forwards through original");
        for (int i = 0; i < 10; ++i) {
            Require(registry.Install(slot, reinterpret_cast<std::uintptr_t>(&Hook), original), "repeat installation is idempotent");
            Require(Invoke(*slot) == 42 && original == &First, "repeat cannot capture itself and recurse");
        }
        *slot = reinterpret_cast<std::uintptr_t>(&Later);
        Require(registry.Install(slot, reinterpret_cast<std::uintptr_t>(&Hook), original) && Invoke(*slot) == 43,
            "repeat preserves a later plugin at head of chain");
        Function unrelated{};
        Require(!registry.Install(slot, reinterpret_cast<std::uintptr_t>(&Foreign), unrelated), "different hook cannot reuse claimed slot");
        auto* second = slot + 1;
        *second = reinterpret_cast<std::uintptr_t>(&Foreign);
        Require(!registry.Install(second, reinterpret_cast<std::uintptr_t>(&Hook), original) && original == &First,
            "different original on second vtable cannot corrupt shared forwarding target");
        *second = reinterpret_cast<std::uintptr_t>(&Hook);
        Require(!registry.Install(second, reinterpret_cast<std::uintptr_t>(&Hook), unrelated), "unknown self-hook rejected");
        std::atomic_int failures{};
        std::vector<std::thread> installers;
        for (int i = 0; i < 8; ++i) {
            installers.emplace_back([&] { if (!registry.Install(slot, reinterpret_cast<std::uintptr_t>(&Hook), original)) { ++failures; } });
        }
        for (auto& thread : installers) { thread.join(); }
        Require(failures == 0 && Invoke(*slot) == 43, "concurrent repeat keeps chain intact");

        DeviceAdmission device;
        std::atomic_int owners{};
        installers.clear();
        for (int i = 0; i < 8; ++i) { installers.emplace_back([&] { if (device.Begin()) { ++owners; } }); }
        for (auto& thread : installers) { thread.join(); }
        Require(owners == 1 && !device.Begin(), "only one device creation can acquire host ownership");

        // Exercise the actual executable's import table, including a prior hook.
        const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto* import = ImportSlot(module, "USER32.dll", "GetClientRect");
        Require(import && !ImportSlot(module, "user32.dll", "NotARealImport"), "resolve import by DLL and symbol");
        SlotRegistry earlier;
        Require(earlier.Install(import, reinterpret_cast<std::uintptr_t>(&EarlierRect), earlierRect), "install earlier plugin import");
        SlotRegistry ours;
        Require(ours.Install(import, reinterpret_cast<std::uintptr_t>(&OurRect), originalRect), "preserve prior IAT target");
        RECT rect{};
        Require(CallRect(&rect) && rect.right == 73 && originalRect == &EarlierRect, "fallback calls earlier plugin, not user32 export");
        Require(ours.Install(import, reinterpret_cast<std::uintptr_t>(&OurRect), originalRect), "IAT hook repeat is safe");
        rect = {};
        Require(CallRect(&rect) && rect.right == 73, "repeat import hook still forwards once");
        std::cout << "PASS: patch validation, import chaining, repeated/concurrent slots and device ownership\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
