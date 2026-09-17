#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "SkyrimRuntime.h"

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
    Require(se && ae640 && ae && se != ae640 && ae640 != ae && se != ae,
        "three admitted runtimes select distinct profiles");

    // Neither family membership nor version ordering admits an unverified game.
    for (const auto version : {
             REL::Version{ 1, 5, 80, 0 }, REL::Version{ 1, 5, 97, 1 },
             REL::Version{ 1, 6, 353, 0 }, REL::Version{ 1, 6, 639, 0 },
             REL::Version{ 1, 6, 640, 1 }, REL::Version{ 1, 6, 641, 0 },
             REL::Version{ 1, 6, 659, 0 }, REL::Version{ 1, 6, 1130, 0 },
             REL::Version{ 1, 6, 1170, 1 }, REL::Version{ 1, 6, 1179, 0 },
             REL::Version{ 1, 7, 104, 0 }, REL::Version{ 1, 4, 15, 0 } }) {
        Require(!SkyrimRuntime::Find(version), "unverified runtime has no profile");
    }

    for (const auto* profile : { se, ae640, ae }) {
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
            for (const auto* other : { se, ae640, ae }) {
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
