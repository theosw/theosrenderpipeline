#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include "HookDetour.h"
#include <iostream>
#include <stdexcept>

using namespace TheosRenderPipeline::HookSafety;
namespace
{
    void Require(bool value, const char* text) { if (!value) { throw std::runtime_error(text); } }
    struct Page
    {
        void* memory = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        Page() { Require(memory != nullptr, "allocate test page"); }
        ~Page() { VirtualFree(memory, 0, MEM_RELEASE); }
        std::uintptr_t At(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(memory) + offset; }
        void Put(std::size_t offset, const void* bytes, std::size_t size) { std::memcpy(reinterpret_cast<void*>(At(offset)), bytes, size); }
    };
    using Function = std::uint64_t (*)(std::uint64_t, std::uint64_t);
    Function original{}, engine{};
    unsigned priorCalls{}, hookCalls{};
    std::uint64_t Prior(std::uint64_t a, std::uint64_t b) { ++priorCalls; return engine(a, b) + 7; }
    std::uint64_t Hook(std::uint64_t a, std::uint64_t b) { ++hookCalls; return original(a, b) + 100; }
    void EntryCase(int kind)
    {
        Page page;
        // mov rax,rcx; add rax,rdx; ret. No PC-relative instructions.
        constexpr std::array<std::uint8_t, 7> body{0x48,0x89,0xC8,0x48,0x01,0xD0,0xC3};
        page.Put(0, body.data(), body.size()); page.Put(128, body.data(), body.size());
        engine = reinterpret_cast<Function>(page.At(128));
        // Earlier hook lives outside the modeled engine image and preserves
        // full-width arguments through a normal C++ function.
        const std::uint8_t jump[]{0x48,0xB8,0,0,0,0,0,0,0,0,0xFF,0xE0};
        page.Put(1024, jump, sizeof(jump));
        const auto prior = reinterpret_cast<std::uintptr_t>(&Prior);
        page.Put(1026, &prior, sizeof(prior));
        const auto destination = page.At(1024);
        if (kind == 1) {
            const std::uint8_t opcode = 0xE9;
            const auto relative = static_cast<std::int32_t>(destination - page.At() - 5);
            page.Put(0, &opcode, 1); page.Put(1, &relative, 4);
        } else if (kind == 2) {
            const std::uint8_t indirect[]{0xFF,0x25,0,0,0,0};
            page.Put(0, indirect, sizeof(indirect)); page.Put(6, &destination, sizeof(destination));
        }
        Require(Entry(page.At(), std::span(body).first(6), page.At(), 512), "entry accepted before installation");
        original = reinterpret_cast<Function>(InstallEntryDetour(page.At(), reinterpret_cast<std::uintptr_t>(&Hook)));
        Require(original != nullptr, "install entry detour");
        constexpr std::uint64_t a = 0x123456789ABC0000, b = 0x987654321;
        priorCalls = hookCalls = 0;
        const auto result = reinterpret_cast<Function>(page.At())(a, b);
        Require(result == a + b + 100 + (kind ? 7 : 0), "forward complete arguments and return value");
        Require(hookCalls == 1 && priorCalls == (kind ? 1u : 0u), "each hook executes once");
    }
    using RectFunction = decltype(&GetClientRect);
    RectFunction rectOriginal{};
    unsigned rectCalls{};
    BOOL WINAPI EarlierRect(HWND window, LPRECT rect)
    {
        ++rectCalls;
        if (reinterpret_cast<std::uintptr_t>(window) != 0x1234567887654321ull || !rect) { return FALSE; }
        rect->right = 73; return TRUE;
    }
    BOOL WINAPI OurRect(HWND window, LPRECT rect) { return rectOriginal(window, rect); }
    void ImportCase()
    {
        Page page;
        // A real FF15 call with shadow space, matching the renderer call form.
        const std::uint8_t wrapper[]{0x48,0x83,0xEC,0x28,0xFF,0x15,0,0,0,0,0x48,0x83,0xC4,0x28,0xC3};
        page.Put(0, wrapper, sizeof(wrapper));
        const auto site = page.At(4), named = page.At(512), alternate = page.At(600);
        auto previous = reinterpret_cast<std::uintptr_t>(&EarlierRect);
        page.Put(512, &previous, sizeof(previous));
        page.Put(600, &previous, sizeof(previous));
        const auto displacement = static_cast<std::int32_t>(alternate - site - 6);
        page.Put(6, &displacement, 4);
        Require(ImportCall(site, named, page.At(), 512), "accept Display Tweaks-style replacement slot");
        Require(!ImportCall(site, 0, page.At(), 512), "named engine import still required");
        const std::uintptr_t invalid = 1;
        page.Put(512, &invalid, sizeof(invalid));
        Require(!ImportCall(site, named, page.At(), 512), "replacement call cannot hide a damaged named import");
        page.Put(512, &previous, sizeof(previous));
        SKSE::Trampoline trampoline;
        trampoline.set_trampoline(reinterpret_cast<void*>(page.At(2048)), 1024);
        // This is the production write_thunk_call<..., 6> forwarding contract.
        rectOriginal = *reinterpret_cast<RectFunction*>(trampoline.write_call<6>(site, reinterpret_cast<std::uintptr_t>(&OurRect)));
        RECT rect{}; rectCalls = 0;
        Require(reinterpret_cast<RectFunction>(page.At())(reinterpret_cast<HWND>(0x1234567887654321ull), &rect) &&
            rect.right == 73 && rectCalls == 1, "replacement import call preserves prior hook and arguments");
        previous = page.At(128); page.Put(600, &previous, sizeof(previous));
        // Restore the original alternate slot call for rejection probes.
        page.Put(0, wrapper, sizeof(wrapper)); page.Put(6, &displacement, 4);
        Require(!ImportCall(site, named, page.At(), 512), "reject unexpected engine-internal callee");
        previous = 1; page.Put(600, &previous, sizeof(previous));
        Require(!ImportCall(site, named, page.At(), 512), "reject nonexecutable target");
        const auto unreadable = static_cast<std::int32_t>(1 - site - 6);
        page.Put(6, &unreadable, 4);
        Require(!ImportCall(site, named, page.At(), 512), "unreadable slot fails closed");
        Require(!InstallEntryDetour(1, reinterpret_cast<std::uintptr_t>(&Hook)), "unreadable entry rejected");
        const std::uint8_t badJump[]{0xFF,0x25,0,0,0,0};
        page.Put(128, badJump, sizeof(badJump)); previous = 1; page.Put(134, &previous, sizeof(previous));
        Require(!InstallEntryDetour(page.At(128), reinterpret_cast<std::uintptr_t>(&Hook)), "malformed entry chain rejected before mutation");
    }
}
int main()
{
    try {
        for (int kind = 0; kind < 3; ++kind) { EntryCase(kind); }
        ImportCase();
        std::cout << "PASS: executable original/E9/FF25 detours and replacement GetClientRect call chain\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
