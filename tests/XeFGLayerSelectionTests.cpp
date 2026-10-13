#include "FrameGen/XeFGLayerSelection.h"
#include <cstdio>
#include <cstdlib>

using namespace TheosRenderPipeline::SourceDLSSG;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }

int main()
{
    unsigned cases = 0;
    auto expect = [&](XeFGLayers got, XeFGLayer hudless, XeFGLayer ui, const char* why) {
        Require(got.hudless == hudless && got.ui == ui, why); ++cases;
    };
    // HDR output composed both layers this frame: XeFG reads its HDR10 copies.
    expect(SelectXeFGLayers(true, true, true, true), XeFGLayer::HDR, XeFGLayer::HDR, "fresh HDR layers");
    // HDR signalled but nothing composed this frame (no UI layer, or not prepared):
    // the HDR targets are stale, so neither is offered and generation holds.
    expect(SelectXeFGLayers(true, true, false, true), XeFGLayer::None, XeFGLayer::None, "stale HDR world image never offered");
    expect(SelectXeFGLayers(true, true, false, false), XeFGLayer::None, XeFGLayer::None, "stale HDR without UI never offered");
    // HDR turned off live (the retained 10-bit swapchain stays, but HDR is not
    // signalled): the standard layers, whose format check then holds generation.
    expect(SelectXeFGLayers(false, true, false, true), XeFGLayer::Standard, XeFGLayer::Standard, "HDR off uses the standard layers");
    expect(SelectXeFGLayers(false, true, true, true), XeFGLayer::Standard, XeFGLayer::Standard, "HDR off ignores an earlier composition");
    // SDR: standard layers; no UI source, no UI layer.
    expect(SelectXeFGLayers(false, false, false, true), XeFGLayer::Standard, XeFGLayer::Standard, "SDR with a UI layer");
    expect(SelectXeFGLayers(false, false, false, false), XeFGLayer::Standard, XeFGLayer::None, "SDR without a UI layer");
    std::printf("PASS: %u XeFG layer selections: fresh HDR, stale HDR refused, HDR off, SDR\n", cases);
}
