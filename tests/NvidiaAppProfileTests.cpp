#include "NvidiaAppProfile.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace TheosRenderPipeline::NvidiaApp;

static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static void Print(const char* name, const std::optional<std::uint32_t>& value)
{
    if (value) { std::printf("  %-20s 0x%08X\n", name, *value); } else { std::printf("  %-20s (not set)\n", name); }
}

int main(int argc, char** argv)
{
    // `TRPNvidiaAppProfileTests <exe name>` prints that game's driver profile; read-only.
    if (argc > 1) {
        const std::string exe(argv[1]);
        const auto values = ReadProfile(std::wstring(exe.begin(), exe.end()));
        std::printf("%s: profile %s\n", argv[1], values.read ? "read" : "unavailable");
        Print("upscalingEnable", values.upscalingEnable); Print("upscalingPreset", values.upscalingPreset);
        Print("upscalingForceDLAA", values.upscalingForceDLAA); Print("generationEnable", values.generationEnable);
        Print("generationPreset", values.generationPreset); Print("generationMode", values.generationMode);
        Print("generationFrames", values.generationFrames); Print("neuralEnable", values.neuralEnable);
        Print("neuralPreset", values.neuralPreset); Print("streamlineEnable", values.streamlineEnable);
        Print("smoothMotion", values.smoothMotion);
        const auto result = Interpret(values);
        std::printf("=> upscaling %d (%s), frame generation %d, NR %d (%s), Smooth Motion %d\n", result.upscaling,
            result.upscalingModel.c_str(), result.frameGeneration, result.neural, result.neuralModel.c_str(), result.smoothMotion);
        return 0;
    }
    // Reading this test's own (absent) profile must not fail without an NVIDIA driver.
    (void)ReadProfile();
    Values none;
    auto result = Interpret(none);
    Require(!result.read && !result.upscaling && !result.frameGeneration && !result.neural, "an unread profile reports nothing");
    Require(result.smoothMotion == -1, "an unread profile leaves Smooth Motion unknown");

    Values clean;
    clean.read = true;
    clean.upscalingEnable = 0;
    clean.upscalingPreset = 0xFFFFFF;   // a preset left behind while the override is off
    clean.generationEnable = 0;
    clean.smoothMotion = 0;
    result = Interpret(clean);
    Require(result.read && !result.upscaling && !result.frameGeneration && !result.neural, "settings with their switch off are no override");
    Require(result.smoothMotion == 0, "Smooth Motion off");

    Values absent;
    absent.read = true;
    Require(Interpret(absent).smoothMotion == 0, "Smooth Motion set nowhere is the driver default, off");

    Values latest = clean;
    latest.upscalingEnable = 1;
    result = Interpret(latest);
    Require(result.upscaling && result.upscalingModel == "Latest", "App's Latest DLSS model");

    Values letter = latest;
    letter.upscalingPreset = 11;
    Require(Interpret(letter).upscalingModel == "K", "preset 11 is K");

    Values dlaa = letter;
    dlaa.upscalingForceDLAA = 1;
    Require(Interpret(dlaa).upscalingModel == "DLAA", "forced DLAA is named");

    Values unnamed = clean;
    unnamed.upscalingEnable = 1;
    unnamed.upscalingPreset = 0;
    Require(Interpret(unnamed).upscalingModel == "NVIDIA App", "an override without a preset is still an override");

    for (auto set : {&Values::generationEnable, &Values::generationMode, &Values::generationFrames, &Values::streamlineEnable}) {
        Values generation = clean;
        generation.*set = 1;
        Require(Interpret(generation).frameGeneration, "each frame-generation override is reported");
    }

    Values neural = clean;
    neural.neuralEnable = 1;
    neural.neuralPreset = 2;
    result = Interpret(neural);
    Require(result.neural && result.neuralModel == "B", "App's NR preset");

    Values smooth = clean;
    smooth.smoothMotion = 1;
    Require(Interpret(smooth).smoothMotion == 1, "Smooth Motion on");

    Require(PresetName(0xFFFFFE) == "NVIDIA App", "unknown preset values are not guessed");
    std::puts("NvidiaAppProfile tests passed");
    return 0;
}
