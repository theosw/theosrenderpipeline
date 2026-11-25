#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "SkyrimRuntime.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    }
}

int main()
{
    using namespace TheosRenderPipeline;
    const auto* se = SkyrimRuntime::Find(REL::Version{ 1, 5, 97, 0 });
    const auto* ae640 = SkyrimRuntime::Find(REL::Version{ 1, 6, 640, 0 });
    const auto* ae = SkyrimRuntime::Find(REL::Version{ 1, 6, 1170, 0 });
    const auto* ae17104 = SkyrimRuntime::Find(REL::Version{ 1, 7, 104, 0 });
    const auto* gog = SkyrimRuntime::Find(REL::Version{ 1, 6, 1179, 0 });
    const std::array all{ se, ae640, ae, ae17104, gog };
    for (std::size_t i = 0; i < all.size(); ++i) {
        Require(all[i] != nullptr, "five admitted runtimes have profiles");
        for (std::size_t j = 0; j < i; ++j) { Require(all[i] != all[j], "admitted runtimes select distinct profiles"); }
    }

    // Steam: SKSE and the executable report the same version. GOG: SKSE sets the
    // distribution (1) in the fourth field; the executable and Address Library say 0.
    for (const auto* steam : { se, ae640, ae, ae17104 }) {
        Require(steam->skse == steam->version && SkyrimRuntime::FindSkse(steam->version) == steam &&
            !SkyrimRuntime::IsGOG(*steam), "Steam SKSE identity is the executable version");
    }
    Require(SkyrimRuntime::FindSkse(REL::Version{ 1, 6, 1179, 1 }) == gog && SkyrimRuntime::IsGOG(*gog),
        "GOG SKSE identity selects the GOG profile");
    Require(gog->skse.pack() == 0x010649B1, "exported GOG metadata is SKSE's RUNTIME_VERSION_1_6_1179_GOG");
    Require(!SkyrimRuntime::FindSkse(REL::Version{ 1, 6, 1179, 0 }) && !SkyrimRuntime::Find(REL::Version{ 1, 6, 1179, 1 }) &&
        !SkyrimRuntime::FindSkse(REL::Version{ 1, 6, 1179, 2 }) && !SkyrimRuntime::FindSkse(REL::Version{ 1, 6, 1170, 1 }),
        "neither identity is masked: each lookup takes only its own form");
    Require(gog->hooks.updateJitter == 0x133 && gog->hooks.rendererClientRect == 0x1DC &&
        gog->hooks.mistBackground == ae->hooks.mistBackground && gog->hooks.renderWorld == ae->hooks.renderWorld &&
        gog->hooks.cameraBranch == ae->hooks.cameraBranch && gog->graphics.runtimeData == 0x60 &&
        gog->graphics.frameCounter == 0x4C && gog->bytes.jitterBranch == ae->bytes.jitterBranch,
        "GOG keeps 1.6.1170 sites and layout except Renderer::Begin's two calls");

    // Neither family membership nor version ordering admits an unverified game.
    for (const auto version : {
             REL::Version{ 1, 5, 80, 0 }, REL::Version{ 1, 5, 97, 1 },
             REL::Version{ 1, 6, 353, 0 }, REL::Version{ 1, 6, 639, 0 },
             REL::Version{ 1, 6, 640, 1 }, REL::Version{ 1, 6, 641, 0 },
             REL::Version{ 1, 6, 659, 0 }, REL::Version{ 1, 6, 1130, 0 },
             REL::Version{ 1, 6, 1170, 1 }, REL::Version{ 1, 6, 1179, 1 },
             REL::Version{ 1, 6, 1179, 2 }, REL::Version{ 1, 6, 1178, 0 },
             REL::Version{ 1, 7, 99, 0 }, REL::Version{ 1, 7, 103, 0 },
             REL::Version{ 1, 7, 104, 1 }, REL::Version{ 1, 7, 105, 0 },
             REL::Version{ 1, 4, 15, 0 } }) {
        Require(!SkyrimRuntime::Find(version), "unverified runtime has no profile");
    }

    for (const auto* profile : all) {
        const auto callers = profile->loadingArtwork;
        for (const auto caller : { callers.exterior, callers.interior }) {
            for (unsigned flags = 0; flags < 64; ++flags) {
                const bool enabled = (flags & 1) != 0;
                const bool ready = (flags & 2) != 0;
                const bool gameCaller = (flags & 4) != 0;
                const auto show = static_cast<std::uint8_t>((flags >> 3) & 1);
                const auto suppress = static_cast<std::uint8_t>((flags >> 4) & 1);
                const auto immediate = static_cast<std::uint8_t>((flags >> 5) & 1);
                const auto expected = enabled && ready && gameCaller && show && suppress && immediate ? 0 : suppress;
                Require(LoadingArtwork::Suppression(callers, enabled, ready, gameCaller,
                            caller, show, suppress, immediate) == expected,
                    "profile preserves artwork setting/readiness/message gates");
            }
            for (const auto foreign : { caller - 1, caller + 1 }) {
                Require(LoadingArtwork::Suppression(callers, true, true, true, foreign, 1, 1, 1) == 1,
                    "adjacent callers preserve suppression");
            }
            for (const auto* other : all) {
                if (other == profile) { continue; }
                for (const auto foreign : { other->loadingArtwork.exterior, other->loadingArtwork.interior }) {
                    Require(LoadingArtwork::Suppression(callers, true, true, true, foreign, 1, 1, 1) == 1,
                        "other-runtime callers preserve suppression");
                }
            }
        }
    }
    std::puts("PASS: exact runtime rejection and artwork profile isolation");
}
