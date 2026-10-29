#include "FrameGen/PresenterReplacement.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace TheosRenderPipeline;
static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

// A fake DXGI window keeps its flip ownership until both the SDK/proxy and
// deferred context references are gone. The production release sequence drives
// this fixture, including partial creations and SDK-retirement failure.
struct Window
{
    int references{2};
    bool deferred{true};
    unsigned creates{}, discards{};
    HRESULT sdkRetirement{S_OK}, drain{S_OK};
    std::vector<std::string> calls;
    HRESULT Release()
    {
        ++discards;
        return ReleasePresenterAfterRetirement([&] { calls.emplace_back("release-native"); --references; }, [&] {
            calls.emplace_back("destroy-sdk");
            if (SUCCEEDED(sdkRetirement)) { --references; }
            return sdkRetirement;
        }, [&] { calls.emplace_back("clear-flush"); deferred = false; }, [&] {
            calls.emplace_back("queue-fence-drain"); return drain;
        });
    }
    HRESULT Create(bool native)
    {
        ++creates;
        calls.emplace_back(native ? "create-native" : "create-sdk");
        if (references || deferred) { return E_ACCESSDENIED; }
        references = 2; deferred = true;
        return S_OK;
    }
};

int main()
{
    Require(XeFGNativeCreationResult("Failed to create swap chain: 0x80070005") == E_ACCESSDENIED,
        "Intel's masked native access-denied HRESULT reaches the retry policy");
    for (const auto* message : {"other error: 0x80070005", "Failed to create swap chain: 0x80070",
        "Failed to create swap chain: 0xGGGGGGGG", "Failed to create swap chain: 0x00000000"}) {
        Require(XeFGNativeCreationResult(message) == E_FAIL, "unknown or malformed diagnostics stay generic");
    }
    {
        Window w;
        Require(w.Create(false) == E_ACCESSDENIED, "the old window owner reproduces failed replacement");
        Require(w.Release() == S_OK && w.Create(false) == S_OK, "SDK release, context flush and queue drain allow HWND reuse");
        Require(w.calls == std::vector<std::string>{"create-sdk", "release-native", "destroy-sdk", "clear-flush", "queue-fence-drain", "create-sdk"},
            "creation follows final SDK release and deferred destruction completion");
    }
    {
        Window w; w.sdkRetirement = E_ACCESSDENIED;
        Require(w.Release() == E_ACCESSDENIED && w.references == 1 && w.deferred && w.calls.size() == 2,
            "failed SDK retirement retains ownership and never flushes/drains/recreates past it");
    }
    {
        PresenterReplacement replacement; replacement.Start();
        unsigned frames{}, requested{}, native{}, cleanup{};
        const auto create = [&](bool fallback) {
            if (fallback) { ++native; return S_OK; }
            return ++requested < 3 ? E_ACCESSDENIED : S_OK;
        };
        const auto discard = [&] { ++cleanup; return S_OK; };
        Require(replacement.Advance(create, discard) == S_FALSE && ++frames == 1 && requested == 1 && cleanup == 1,
            "first refusal returns to the game for a later-frame retry, without a fault");
        Require(replacement.Advance(create, discard) == S_FALSE && ++frames == 2 && requested == 2,
            "one attempt per following frame");
        Require(replacement.Advance(create, discard) == S_OK && ++frames == 3 && !replacement.Pending() && native == 0,
            "transient window ownership recovers the requested presenter");
    }
    for (const auto error : {E_ACCESSDENIED, E_FAIL, E_INVALIDARG}) {
        PresenterReplacement replacement; replacement.Start();
        unsigned requested{}, native{}, cleanup{};
        bool displayed{};
        const auto create = [&](bool fallback) {
            if (fallback) { ++native; displayed = true; return S_OK; }
            ++requested; return error;
        };
        const auto discard = [&] { ++cleanup; return S_OK; };
        for (unsigned frame = 0; replacement.Pending() && frame < 8; ++frame) {
            Require(SUCCEEDED(replacement.Advance(create, discard)), "requested creation refusal stays recoverable");
        }
        Require(!replacement.Pending() && replacement.Native() && displayed && native == 1 && cleanup == requested &&
            requested == (PresenterReplacement::Retryable(error) ? PresenterReplacement::MaxAttempts : 1),
            "bounded failed provider attempts fall back to a displaying native presenter");
        Require(replacement.FirstFailure() == error, "fallback preserves the original creation failure evidence");
    }
    {
        Window w;
        Require(w.Release() == S_OK, "retire old presenter before injected partial creation");
        PresenterReplacement replacement; replacement.Start();
        const auto create = [&](bool native) {
            const auto result = w.Create(native);
            // Fail after the SDK has acquired native ownership on attempt one.
            return w.creates == 1 && SUCCEEDED(result) ? E_ACCESSDENIED : result;
        };
        Require(replacement.Advance(create, [&] { return w.Release(); }) == S_FALSE && !w.references && !w.deferred,
            "a failed partial SDK creation releases every native/deferred reference before retry");
        Require(replacement.Advance(create, [&] { return w.Release(); }) == S_OK && w.creates == 2,
            "the next frame can reuse the HWND after partial-publication cleanup");
    }
    {
        PresenterReplacement replacement; replacement.Start();
        unsigned native{}, cleanup{};
        const auto create = [&](bool fallback) { if (fallback) { ++native; } return E_ACCESSDENIED; };
        const auto discard = [&] { ++cleanup; return S_OK; };
        HRESULT result{};
        for (unsigned frame = 0; frame < 8; ++frame) {
            result = replacement.Advance(create, discard);
            if (FAILED(result)) { break; }
        }
        Require(result == E_ACCESSDENIED && native == PresenterReplacement::MaxAttempts && cleanup == 6,
            "native DXGI refusal also has a finite budget and preserves the actual error");
    }
    for (const auto retirement : {E_FAIL, DXGI_ERROR_DEVICE_REMOVED}) {
        PresenterReplacement replacement; replacement.Start(); unsigned attempts{};
        Require(replacement.Advance([&](bool) { ++attempts; return E_ACCESSDENIED; }, [&] { return retirement; }) == retirement &&
            attempts == 1 && !replacement.Native(), "failed partial-owner retirement never authorizes another creation");
    }
    for (const auto terminal : {E_UNEXPECTED, DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_DRIVER_INTERNAL_ERROR}) {
        PresenterReplacement replacement; replacement.Start(); bool discarded{};
        Require(replacement.Advance([&](bool) { return terminal; }, [&] { discarded = true; return S_OK; }) == terminal &&
            !discarded, "unsafe state/device loss retains ownership without pretending native fallback can repair it");
    }
    std::puts("PASS presenter destruction ordering, later-frame retries, bounded native fallback and failure retention");
}
