#pragma once
#include <chrono>
#include <filesystem>
#include <string>
#include "PhotoFileMatch.h"

// Skyrim saves screenshots from kSCREENSHOT, its own target at the render size
// TRP gives the game: no DLSS output, no NR, no native UI. TRP replaces such a
// file with the finished frame of the same Present, as it does for ReShade's
// screenshots.
//
// PrintScreen: Skyrim's SaveRenderTargetToFile writes the file and its path is
// queued. Photo Mode raises the same screenshot flag but writes its own photo
// (My Games\<game>\Photos\Screenshot_N.png) without that call; the flag queues a
// capture anyway, and the photo it finds is replaced (FindPhoto). Photo Mode
// takes its photo in Main::Swap and the flag is clear again before Present, so
// the flag is also sampled when the frame starts (OnFrameBegin).
namespace TheosRenderPipeline::EngineScreenshot
{
    struct Request
    {
        std::string path;  // empty: find the photo another plugin writes (FindPhoto)
        int jpegQuality{ 90 };
        PhotoFiles::Request photo;
        unsigned width{}, height{};  // kSCREENSHOT's size when the flag rose
    };

    // Hooks BSGraphics::Renderer::SaveRenderTargetToFile. Declines (and logs the
    // entry bytes) when the function's entry cannot be detoured safely; Skyrim's
    // own screenshots are then unchanged.
    void Install();
    // At each D3D11 Present, before that frame is finished: a rising screenshot
    // flag queues a capture of the next finished frame.
    void OnPresent();
    // When Skyrim starts rendering a frame: remembers a set screenshot flag for
    // that frame's OnPresent.
    void OnFrameBegin();
    // The oldest screenshot waiting for the finished frame.
    bool Take(Request& a_request);
    // Worker-only conservative match: one newly created, completed PNG at the
    // requested dimensions. Holds it against writes; ambiguity leaves it intact.
    PhotoFiles::File FindPhoto(const Request& a_request, std::chrono::milliseconds a_timeout);
}
