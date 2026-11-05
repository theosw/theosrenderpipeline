#include <PCH.h>
#include "NeuralObjectHooks.h"
#include "NeuralObjectHookContract.h"
#include "HookDetour.h"
#include "FrameGen/NvidiaHost.h"
#include <Psapi.h>
#include <array>
#include <filesystem>

namespace TheosRenderPipeline::NeuralObjects
{
    namespace
    {
        using Group = NeuralRendering::ObjectGroup;
        // The verified 1.5.97 and 1.6.1170 ABI is (pass, technique, alphaTest, flags).
        // It is a static draw helper, despite the member declaration in CommonLib.
        using DrawPass = void(*)(RE::BSRenderPass*, std::uint32_t, bool, std::uint32_t);
        DrawPass original{};
        std::atomic<bool> enabled{};
        thread_local Group currentGroup{};
        NeuralRendering::ObjectCapture capture;
        std::array<bool,4> loggedGroups{};
        std::atomic<const char*> status{"Object capture is unavailable on this Skyrim version"};
        // Set when installing was refused, so the rows grey out instead of retrying.
        std::atomic<bool> declined{};
        bool SupportedVersion() { return DrawHook::ForVersion(REL::Module::get().version()) != nullptr; }
        // Where draws leave the capture path, summarized in the log while masks are on.
        using Skip = NeuralRendering::ObjectCapture::Skip;
        struct Counters
        {
            std::atomic<std::uint64_t> passes, eligible, draws, tagged, hostSkipped, captured;
            std::array<std::atomic<std::uint64_t>, 5> groups;
            std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Skip::Count)> skips;
        };
        Counters counters;
        std::array<bool, static_cast<std::size_t>(Skip::Count)> detailLogged{};
        std::atomic<std::uint64_t> lastSummary{};
        void Summarize(std::uint64_t frame)
        {
            auto previous = lastSummary.load(std::memory_order_relaxed);
            if (frame < previous + 600 || !lastSummary.compare_exchange_strong(previous, frame)) { return; }
            auto n = [](const std::atomic<std::uint64_t>& value) { return value.load(std::memory_order_relaxed); };
            std::string skipped;
            for (std::size_t i = 1; i < counters.skips.size(); ++i) {
                if (n(counters.skips[i])) { skipped += std::format(" {}={}", NeuralRendering::ObjectCapture::kSkipNames[i], n(counters.skips[i])); }
            }
            logger::info("[NR Objects] frame={} passes={} eligible={} classified world={} firstPerson={} player={} humanoid={} creature={} "
                "draws={} tagged={} hostSkipped={} captured={} skipped:{}", frame, n(counters.passes), n(counters.eligible),
                n(counters.groups[0]), n(counters.groups[1]), n(counters.groups[2]), n(counters.groups[3]), n(counters.groups[4]),
                n(counters.draws), n(counters.tagged), n(counters.hostSkipped), n(counters.captured), skipped.empty() ? " none" : skipped);
        }

        Group Classify(RE::BSGeometry* geometry)
        {
            if (!geometry) { return Group::World; }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) { return Group::World; }
            // The pass owns live geometry. Retain the starting node and inspect
            // a bounded ancestry; do not follow unrelated cached scene pointers.
            RE::NiPointer<RE::NiAVObject> retained{geometry};
            RE::NiPointer<RE::NiAVObject> firstPerson{player->Get3D(true)};
            RE::TESObjectREFR* owner{};
            std::array<RE::NiAVObject*, 64> seen{};
            auto* node = retained.get();
            for (unsigned depth = 0; node && depth < seen.size(); ++depth) {
                if (std::find(seen.begin(), seen.begin()+depth, node) != seen.begin()+depth) { return Group::World; }
                seen[depth] = node;
                if (firstPerson && node == firstPerson.get()) { return Group::FirstPerson; }
                if (!owner && node->userData) { owner = node->userData; }
                node = node->parent;
            }
            if (!owner) { return Group::World; }
            if (owner == player) { return Group::Player; }
            RE::NiPointer<RE::TESObjectREFR> retainedOwner{owner};
            auto* actor = retainedOwner->As<RE::Actor>();
            if (!actor) { return Group::World; }
            auto* race = actor->GetRace();
            return race && race->HasKeywordString("ActorTypeNPC") ? Group::Humanoid : Group::Creature;
        }
        void Pass(RE::BSRenderPass* pass, std::uint32_t technique, bool alphaTest, std::uint32_t flags)
        {
            const auto previous = currentGroup;
            const bool on = enabled.load(std::memory_order_relaxed);
            const bool eligible = on && pass && pass->shader && pass->shader->shaderType == RE::BSShader::Type::Lighting && !alphaTest;
            currentGroup = eligible ? Classify(pass->geometry) : Group::World;
            if (on) {
                counters.passes.fetch_add(1, std::memory_order_relaxed);
                if (eligible) {
                    counters.eligible.fetch_add(1, std::memory_order_relaxed);
                    counters.groups[static_cast<std::size_t>(currentGroup)].fetch_add(1, std::memory_order_relaxed);
                }
            }
            original(pass, technique, alphaTest, flags);
            currentGroup = previous;
        }
    }
    void Install()
    {
        const auto* contract = DrawHook::ForVersion(REL::Module::get().version());
        if (original || !contract) { return; }
        const auto site = REL::ID(contract->relocationID).address();
        // Verified in the retained, exact-version offline executables. SSE Engine
        // Fixes already replaces this entry with a jump out of the image; chain
        // behind such a plugin. Any other change is an unknown patch: decline it.
        MODULEINFO image{};
        if (!K32GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &image, sizeof(image)) ||
            !HookSafety::Entry(site, contract->prefix, REL::Module::get().base(), image.SizeOfImage)) {
            status.store("Object capture hook conflicts with another patch; normal NR retained");
            declined.store(true);
            logger::warn("[NR Objects] draw-helper entry is neither the original code nor another plugin's jump; capture unavailable");
            return;
        }
        if (const auto previous = HookSafety::EntryJumpTarget(site)) {
            // A plugin stub usually forwards once more (SKSE trampoline); name the final owner.
            const auto owner = HookSafety::EntryJumpTarget(previous) ? HookSafety::EntryJumpTarget(previous) : previous;
            HMODULE module{};
            std::array<wchar_t, MAX_PATH> path{};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(owner), &module)) { GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())); }
            logger::info("[NR Objects] draw-helper entry already patched by {}; chaining behind it",
                path[0] ? std::filesystem::path(path.data()).filename().string() : "another plugin");
        }
        original = reinterpret_cast<DrawPass>(HookSafety::InstallEntryDetour(site, reinterpret_cast<std::uintptr_t>(&Pass)));
        status.store(original ? "Experimental opaque-object capture ready" : "Object capture hook unavailable; normal NR retained");
        declined.store(!original);
    }
    void Configure(bool value)
    {
        // Settings are applied outside the game's scene-draw scope. Leave the
        // engine entry point untouched until this feature is explicitly enabled.
        if (value && !original) { Install(); }
        enabled.store(value && original, std::memory_order_relaxed);
    }
    bool Active() { return enabled.load(std::memory_order_relaxed); }
    void ResetAfterRetirement()
    {
        capture.Reset(); loggedGroups = {};
    }
    void RecordDraw(ID3D11DeviceContext* context, const std::function<void()>& draw)
    {
        if (!enabled.load(std::memory_order_relaxed)) { return; }
        auto* host = NvidiaHost::GetSingleton();
        counters.draws.fetch_add(1, std::memory_order_relaxed);
        Summarize(host->PresentCount());
        if (currentGroup == Group::World) { return; }
        counters.tagged.fetch_add(1, std::memory_order_relaxed);
        if (!host->UpscalerReady() || !host->SourceContext(context) || host->NativeUIPassActive()) {
            counters.hostSkipped.fetch_add(1, std::memory_order_relaxed); return;
        }
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) { counters.hostSkipped.fetch_add(1, std::memory_order_relaxed); return; }
        const auto& targets = renderer->GetDepthStencilData().depthStencils;
        const FrameExtent active{host->RenderWidth(), host->RenderHeight()};
        const auto hr = capture.Record(context, currentGroup, host->PresentCount(), active,
            reinterpret_cast<ID3D11Resource*>(targets[0].texture), reinterpret_cast<ID3D11Resource*>(targets[7].texture), draw);
        if (hr == S_OK) { counters.captured.fetch_add(1, std::memory_order_relaxed); }
        else if (hr == S_FALSE) {
            const auto skip = static_cast<std::size_t>(capture.LastSkip());
            if (skip < counters.skips.size()) {
                counters.skips[skip].fetch_add(1, std::memory_order_relaxed);
                if (!detailLogged[skip]) {
                    detailLogged[skip] = true;
                    const auto viewport = capture.LastViewport();
                    logger::info("[NR Objects] first skip reason={} active={}x{} viewport={}x{} at {},{} count={}",
                        NeuralRendering::ObjectCapture::kSkipNames[skip], active.width, active.height,
                        viewport.Width, viewport.Height, viewport.TopLeftX, viewport.TopLeftY, capture.LastViewportCount());
                }
            }
        }
        const auto group = static_cast<unsigned>(currentGroup)-1;
        if (hr == S_OK && group < loggedGroups.size() && !loggedGroups[group]) {
            loggedGroups[group] = true;
            logger::info("[NR Objects] captured opaque draw group={} frame={}", NeuralRendering::kObjectGroups[group], host->PresentCount());
        }
        if (FAILED(hr)) {
            enabled.store(false); status.store("Object capture failed; normal NR retained until Apply");
            logger::warn("[NR Objects] capture failed hr=0x{:08X}; normal NR retained", static_cast<unsigned>(hr));
        }
    }
    ID3D11Texture2D* Current()
    {
        return enabled.load(std::memory_order_relaxed) ? capture.Current(NvidiaHost::GetSingleton()->PresentCount()) : nullptr;
    }
    const char* Status()
    {
        // The hook installs only when a group is first switched on.
        if (!original && !declined.load() && SupportedVersion()) { return "Object capture starts when you switch a group on"; }
        return status.load();
    }
    bool Available() { return original != nullptr || (SupportedVersion() && !declined.load()); }
}
