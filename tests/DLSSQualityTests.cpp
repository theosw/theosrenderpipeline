#include <d3d11.h>
#include "DLSSFeatureParameters.h"
#include "FrameGen/SourceDLSSGCamera.h"
#include "NvidiaUpscalerConfiguration.h"
#include <cstdio>
#include <cstdlib>

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

int main()
{
    using namespace TheosRenderPipeline;
    constexpr int outputs[][2]{{5120,1440}, {1920,1080}, {2560,1440}, {3840,2160}, {1919,1079}};
    constexpr int expected[][2]{{2560,720}, {2970,835}, {3413,960}, {1707,480}, {3982,1120}, {5120,1440}};
    constexpr NVSDK_NGX_PerfQuality_Value modes[]{
        NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_Balanced,
        NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_UltraPerformance,
        NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_DLAA};

    for (const auto& output : outputs) for (int quality = 0; quality < 6; ++quality) {
        Upscaler::Configuration configuration;
        configuration.Initialize({0,2,11,true,true});
        configuration.Request({quality == 5 ? 3 : 0, quality == 5 ? 2 : quality,11,true,true});
        Require(configuration.LiveCandidate().AllocationQuality() == 2, "scale edits retain the live allocation until restart");
        configuration.Saved();
        const auto saved = configuration.Persisted();
        configuration.Initialize(saved);
        const int allocation = configuration.Startup().AllocationQuality();
        Require(allocation == quality && !configuration.NeedsRestart(), "saved quality survives restart without downgrade");
        int w{}, h{};
        Require(SourceDLSSG::QueryRenderSize(output[0], output[1], allocation, &w, &h), "saved quality allocates successfully");
        Require(w > 0 && h > 0 && w <= output[0] && h <= output[1], "valid bounded render extent");
        if (output[0] == 5120) Require(w == expected[quality][0] && h == expected[quality][1], "original scale dimensions preserved");
        for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
            const auto params = DLSS::CreationParameters(w, h, output[0], output[1], format, allocation, true, true);
            Require(params.Feature.InWidth == w && params.Feature.InHeight == h &&
                params.Feature.InTargetWidth == output[0] && params.Feature.InTargetHeight == output[1], "NGX receives the host's published extent");
            Require(params.Feature.InPerfQualityValue == modes[quality], "saved mode selects a supported NGX enum");
            Require(bool(params.InFeatureCreateFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) == (quality != 5), "DLAA and scaled motion-vector flags preserved");
            Require(bool(params.InFeatureCreateFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) == (format == DXGI_FORMAT_R16G16B16A16_FLOAT), "input format contract preserved");
            Require(!params.InEnableOutputSubrects, "fixed output contract preserved");
        }
    }
    std::puts("PASS: 30 saved-allocation/restart cases and 60 SDR/FP16 creation contracts");
}
