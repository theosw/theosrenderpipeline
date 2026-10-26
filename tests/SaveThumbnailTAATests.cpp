#include "SaveThumbnailTAA.h"
#include <cstdio>
#include <cstdlib>

using TheosRenderPipeline::SaveThumbnailTAA;
using Event = SaveThumbnailTAA::Event;

static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

int main()
{
    // No request: TAA stays off and nothing is logged.
    {
        SaveThumbnailTAA taa;
        for (int i = 0; i < 4; ++i) {
            const auto d = taa.Next(false);
            Require(!d.taa && d.event == Event::None, "TAA stays off without a request");
        }
    }
    // A quicksave: TAA runs while the request is set, then goes off again.
    {
        SaveThumbnailTAA taa;
        auto d = taa.Next(true);
        Require(d.taa && d.event == Event::Started && d.frames == 1, "the request lets TAA run");
        d = taa.Next(true);
        Require(d.taa && d.event == Event::None && d.frames == 2, "still drawing");
        d = taa.Next(false);
        Require(!d.taa && d.event == Event::Finished && d.frames == 2, "the thumbnail is drawn; TAA off");
        d = taa.Next(true);
        Require(d.taa && d.event == Event::Started, "the next save starts again");
    }
    // A request TAA does not clear: TAA goes back off after kGiveUpFrames and
    // stays off until the request clears, which ends quietly.
    {
        SaveThumbnailTAA taa;
        for (unsigned i = 1; i <= SaveThumbnailTAA::kGiveUpFrames; ++i) {
            Require(taa.Next(true).taa, "TAA runs up to the limit");
        }
        auto d = taa.Next(true);
        Require(!d.taa && d.event == Event::GaveUp && d.frames == SaveThumbnailTAA::kGiveUpFrames + 1, "gives up once");
        d = taa.Next(true);
        Require(!d.taa && d.event == Event::None, "stays off while the request lasts");
        d = taa.Next(false);
        Require(!d.taa && d.event == Event::None, "a request given up on clears without a Finished event");
        Require(taa.Next(true).taa, "a new request tries again");
    }
    std::puts("Save thumbnail TAA: no request, a quicksave, and a request TAA does not clear passed.");
}
