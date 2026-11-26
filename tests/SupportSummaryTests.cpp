#include "SupportSummary.h"
#include <cstdio>
#include <cstdlib>

using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::Support;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
static bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

int main()
{
    Summary summary;
    summary.release = "0.7.4 Beta 1"; summary.edition = "Universal"; summary.source = "52d52aa";
    summary.skyrim = "1.6.1170.0";
    summary.gpu = "NVIDIA GeForce RTX 3060"; summary.driver = "566.36 (32.0.15.6636)";
    summary.upscaling = "DLSS | 1708x960 -> 2560x1440 | ready | preset K";
    summary.frameGeneration = "NVIDIA DLSS-G | inactive | requested on";
    summary.neuralRendering = "on | inactive | before upscaling | 2 passes";
    summary.nvidiaApp = {"Skyrim's driver profile was read", "DLSS model override: Latest", "Smooth Motion: off"};
    summary.degraded = {
        {Degradation::Reason::FrameGenerationStartup, "NVIDIA driver 566.36 is too old for RTX 20/30 frame generation failed HRESULT=0x887A0004"},
        {Degradation::Reason::GuideRefused, "depth copy refused", false},
        {Degradation::Reason::LoadingArtworkUnavailable, "Loading artwork hook does not match the engine transition sender"}};
    summary.modules = {
        {"nvngx_dlss.dll", "310.4.0.0", "C:\\Users\\Player\\MO2\\mods\\TRP\\SKSE\\Plugins\\TheosRenderPipeline\\NVIDIA\\nvngx_dlss.dll"},
        {"NvPresent64.dll", "", "C:\\WINDOWS\\System32\\DriverStore\\NvPresent64.dll"}};
    summary.userProfile = "C:\\Users\\Player";

    const auto text = Format(summary);
    Require(Has(text, "TRP: 0.7.4 Beta 1 | Universal | source 52d52aa\n"), "TRP version, edition and source");
    Require(Has(text, "Skyrim: 1.6.1170.0\n") && Has(text, "GPU: NVIDIA GeForce RTX 3060 | driver 566.36 (32.0.15.6636)\n"), "game, GPU and driver");
    Require(Has(text, "Upscaling: DLSS | 1708x960 -> 2560x1440 | ready | preset K\n") &&
        Has(text, "Frame generation: NVIDIA DLSS-G | inactive | requested on\n") &&
        Has(text, "Neural Rendering: on | inactive | before upscaling | 2 passes\n"), "what is in effect");
    Require(Has(text, "NVIDIA App / driver:\n- Skyrim's driver profile was read\n- DLSS model override: Latest\n- Smooth Motion: off\n"),
        "NVIDIA App and driver override state");
    Require(Has(text, "Off or reduced this session:\n- frame generation: frame generation did not start | NVIDIA driver 566.36 is too old "
        "for RTX 20/30 frame generation failed HRESULT=0x887A0004\n- loading artwork: loading artwork unavailable | Loading artwork hook "
        "does not match the engine transition sender\n"), "every active degraded reason with its technical status");
    Require(Has(text, "Earlier this session, since restored:\n- frame generation: a frame-generation input copy was refused | depth copy refused\n"),
        "a resolved degradation is still named, apart from the active ones");
    Require(Has(text, "- nvngx_dlss.dll 310.4.0.0 | %USERPROFILE%\\MO2\\mods\\TRP\\SKSE\\Plugins\\TheosRenderPipeline\\NVIDIA\\nvngx_dlss.dll\n") &&
        Has(text, "- NvPresent64.dll | C:\\WINDOWS\\System32\\DriverStore\\NvPresent64.dll\n"), "loaded runtime files with versions where known");
    Require(!Has(text, "Player"), "the Windows user name is not in the text");

    // Nothing known, nothing degraded.
    {
        const auto empty = Format({});
        Require(Has(empty, "TRP: unknown | unknown | source unknown\n") && Has(empty, "GPU: unknown | driver unknown\n") &&
            Has(empty, "NVIDIA App / driver: not read\n") && Has(empty, "Off or reduced this session: nothing\n") &&
            Has(empty, "Loaded runtime files: none\n") && !Has(empty, "since restored"), "an empty summary says what is unknown");
        Summary single; single.nvidiaApp = {"not an NVIDIA GPU; driver profile not read"};
        Require(Has(Format(single), "NVIDIA App / driver: not an NVIDIA GPU; driver profile not read\n"), "one NVIDIA App line stays on its row");
    }

    {
        Summary aside; aside.steppedAside = true;
        Require(Has(Format(aside), "Off or reduced this session:\n- everything: TRP stepped aside for NVIDIA Smooth Motion"),
            "a step-aside session is not reported as nothing off");
    }

    // The user folder is shortened wherever it appears, in any case and slash style.
    Require(RedactUserFolder("c:/users/player/Documents/x.dll and C:\\USERS\\PLAYER\\y", "C:\\Users\\Player") ==
        "%USERPROFILE%/Documents/x.dll and %USERPROFILE%\\y", "case and slash variants");
    Require(RedactUserFolder("C:\\Users\\Players\\x.dll", "C:\\Users\\Player") == "C:\\Users\\Players\\x.dll", "another folder with the same start is kept");
    Require(RedactUserFolder("C:\\Users\\Player", "C:\\Users\\Player") == "%USERPROFILE%", "the folder itself");
    Require(RedactUserFolder("(C:\\Users\\Player)\nC:\\Users\\Player,other;C:\\Users\\Player", "C:\\Users\\Player") ==
        "(%USERPROFILE%)\n%USERPROFILE%,other;%USERPROFILE%", "punctuation and newline boundaries");
    Require(RedactUserFolder("C:\\Games\\x.dll", "") == "C:\\Games\\x.dll" && RedactUserFolder("C:\\Games\\x.dll", "C:\\") == "C:\\Games\\x.dll",
        "an unknown or implausibly short profile changes nothing");
    std::puts("SupportSummary tests passed");
    return 0;
}
