#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include "HookDetour.h"
#include "NeuralObjectHookContract.h"
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
    // SSE Engine Fixes' shape: its entry patch is E9 rel32 plus a nop over the
    // first two instructions, and its original path runs those instructions
    // elsewhere and resumes at entry+6. Our 5-byte detour must leave that intact.
    Function resume{};
    std::uint64_t Resuming(std::uint64_t a, std::uint64_t b) { ++priorCalls; return resume(a, b) + 7; }
    void ResumeCase()
    {
        Page page;
        // mov rax,rcx; add rax,rdx; ret
        constexpr std::array<std::uint8_t, 7> body{0x48,0x89,0xC8,0x48,0x01,0xD0,0xC3};
        page.Put(0, body.data(), body.size());
        // The predecessor's original: both relocated instructions, then jmp entry+6.
        page.Put(256, body.data(), 6);
        const std::uint8_t back = 0xE9;
        const auto backRelative = static_cast<std::int32_t>(page.At(6) - page.At(262) - 5);
        page.Put(262, &back, 1); page.Put(263, &backRelative, 4);
        resume = reinterpret_cast<Function>(page.At(256));
        const std::uint8_t stub[]{0x48,0xB8,0,0,0,0,0,0,0,0,0xFF,0xE0};
        page.Put(1024, stub, sizeof(stub));
        const auto handler = reinterpret_cast<std::uintptr_t>(&Resuming);
        page.Put(1026, &handler, sizeof(handler));
        const std::uint8_t entry[]{0xE9,0,0,0,0,0x90};
        const auto relative = static_cast<std::int32_t>(page.At(1024) - page.At() - 5);
        page.Put(0, entry, sizeof(entry)); page.Put(1, &relative, 4);
        Require(Entry(page.At(), std::span(body).first(6), page.At(), 512), "Engine Fixes-style entry accepted");
        original = reinterpret_cast<Function>(InstallEntryDetour(page.At(), reinterpret_cast<std::uintptr_t>(&Hook)));
        Require(original != nullptr, "chain after an E9+nop entry patch");
        std::array<std::uint8_t, 2> tail{};
        std::memcpy(tail.data(), reinterpret_cast<const void*>(page.At(5)), tail.size());
        Require(tail[0] == 0x90 && tail[1] == 0xC3, "detour leaves the predecessor's resume point intact");
        constexpr std::uint64_t a = 0x123456789ABC0000, b = 0x987654321;
        priorCalls = hookCalls = 0;
        Require(reinterpret_cast<Function>(page.At())(a, b) == a + b + 7 + 100, "hook, predecessor and resumed engine body all run");
        Require(hookCalls == 1 && priorCalls == 1, "each layer runs once");
    }
    using DrawPass = void (*)(RE::BSRenderPass*, std::uint32_t, bool, std::uint32_t);
    DrawPass drawOriginal{}, drawEngine{};
    RE::BSRenderPass* observedPass{};
    std::uint32_t observedTechnique{}, observedFlags{};
    bool observedAlpha{};
    void DrawEngine(RE::BSRenderPass* pass, std::uint32_t technique, bool alpha, std::uint32_t flags)
    {
        observedPass = pass; observedTechnique = technique; observedAlpha = alpha; observedFlags = flags;
    }
    void DrawPrior(RE::BSRenderPass* pass, std::uint32_t technique, bool alpha, std::uint32_t flags)
    {
        ++priorCalls; drawEngine(pass, technique, alpha, flags);
    }
    void DrawHook(RE::BSRenderPass* pass, std::uint32_t technique, bool alpha, std::uint32_t flags)
    {
        ++hookCalls; drawOriginal(pass, technique, alpha, flags);
    }
    void ObjectDrawCase(REL::Version version, int kind)
    {
        namespace Contract = TheosRenderPipeline::NeuralObjects::DrawHook;
        const auto* contract = Contract::ForVersion(version);
        Require(contract != nullptr, "verified object-draw version admitted");
        Page page;
        // Execute the exact production prefix, then restore its stack/registers
        // and tail-call a recorder. This exercises Detours' displaced prologue.
        const std::uint8_t seRestore[]{0x48,0x83,0xc4,0x20,0x41,0x5f,0x41,0x5e,0x5e,
            0x48,0x8b,0x6c,0x24,0x18,0x48,0x8b,0x5c,0x24,0x10};
        const std::uint8_t aeRestore[]{0x48,0x81,0xc4,0x80,0,0,0,0x41,0x5f,0x41,0x5e,
            0x41,0x5d,0x41,0x5c,0x5f};
        const auto restore = version == REL::Version{1,5,97,0}
            ? std::span<const std::uint8_t>(seRestore) : std::span<const std::uint8_t>(aeRestore);
        const std::uint8_t jump[]{0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
        const auto recorder = reinterpret_cast<std::uintptr_t>(&DrawEngine);
        for (const auto offset : {0u, 128u}) {
            page.Put(offset, contract->prefix.data(), contract->prefix.size());
            auto tail = offset + contract->prefix.size();
            page.Put(tail, restore.data(), restore.size()); tail += restore.size();
            page.Put(tail, jump, sizeof(jump)); page.Put(tail+2, &recorder, sizeof(recorder));
        }
        const auto site = page.At();
        Require(Entry(site, contract->prefix, site, 512), "exact version-specific draw prefix accepted");
        const auto damaged = static_cast<std::uint8_t>(contract->prefix.back() ^ 1);
        page.Put(contract->prefix.size()-1, &damaged, 1);
        Require(!Entry(site, contract->prefix, site, 512), "changed final draw-prefix byte rejected");
        page.Put(0, contract->prefix.data(), contract->prefix.size());
        drawEngine = reinterpret_cast<DrawPass>(page.At(128));
        const auto prior = reinterpret_cast<std::uintptr_t>(&DrawPrior);
        page.Put(1024, jump, sizeof(jump)); page.Put(1026, &prior, sizeof(prior));
        const auto predecessor = page.At(1024);
        if (kind == 1) {
            const std::uint8_t entry[]{0xe9,0,0,0,0};
            const auto relative = static_cast<std::int32_t>(predecessor-site-5);
            page.Put(0, entry, sizeof(entry)); page.Put(1, &relative, sizeof(relative));
        } else if (kind == 2) {
            const std::uint8_t entry[]{0xff,0x25,0,0,0,0};
            page.Put(0, entry, sizeof(entry)); page.Put(6, &predecessor, sizeof(predecessor));
        }
        Require(Entry(site, contract->prefix, site, 512), "draw entry accepts original or executable predecessor");
        drawOriginal = reinterpret_cast<DrawPass>(InstallEntryDetour(site, reinterpret_cast<std::uintptr_t>(&DrawHook)));
        Require(drawOriginal != nullptr, "install guarded four-argument draw hook");
        const auto pass = reinterpret_cast<RE::BSRenderPass*>(0x123456789abc0000ull);
        for (const bool alpha : {false, true}) {
            hookCalls = priorCalls = 0;
            reinterpret_cast<DrawPass>(site)(pass, 0xfedcba98u, alpha, 0x87654321u);
            Require(observedPass == pass && observedTechnique == 0xfedcba98u && observedAlpha == alpha &&
                observedFlags == 0x87654321u, "draw hook preserves pointer and all three scalar arguments");
            Require(hookCalls == 1 && priorCalls == (kind ? 1u : 0u), "draw chain calls each hook once");
        }
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
        ResumeCase();
        namespace Contract = TheosRenderPipeline::NeuralObjects::DrawHook;
        Require(Contract::ForVersion({1,5,97,0})->relocationID == 100854 &&
            Contract::ForVersion({1,6,1170,0})->relocationID == 107644, "draw hooks select the exact-version address ID");
        for (const auto version : {REL::Version{1,6,640,0}, REL::Version{1,7,104,0}, REL::Version{1,5,97,1}}) {
            Require(!Contract::ForVersion(version), "unverified draw-hook versions remain unavailable");
        }
        for (int kind = 0; kind < 3; ++kind) {
            ObjectDrawCase({1,5,97,0}, kind); ObjectDrawCase({1,6,1170,0}, kind);
        }
        ImportCase();
        std::cout << "PASS: executable original/E9/FF25 detours, an Engine Fixes-style resuming predecessor, and replacement GetClientRect call chain\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
