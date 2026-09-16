#pragma once

#include <dxgiformat.h>
#include <nvsdk_ngx_helpers.h>

namespace TheosRenderPipeline::DLSS
{
    inline bool IsHDRFormat(DXGI_FORMAT format)
    {
        switch (format) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return true;
        default:
            return false;
        }
    }

    // Input format comes from the host's allocation, including on recreation.
    // Quality values retain the INI contract; 5 is the native DLAA allocation.
    inline NVSDK_NGX_DLSS_Create_Params CreationParameters(unsigned renderWidth, unsigned renderHeight,
        unsigned outputWidth, unsigned outputHeight, DXGI_FORMAT inputFormat,
        int quality, bool sharpening, bool autoExposure)
    {
        NVSDK_NGX_DLSS_Create_Params result{};
        result.Feature.InWidth = renderWidth;
        result.Feature.InHeight = renderHeight;
        result.Feature.InTargetWidth = outputWidth;
        result.Feature.InTargetHeight = outputHeight;
        switch (quality) {
        case 0: result.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
        case 1: result.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced; break;
        case 3: result.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
        case 4: result.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_UltraQuality; break;
        case 5: result.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA; break;
        default: result.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
        }
        if (autoExposure) { result.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure; }
        if (IsHDRFormat(inputFormat)) { result.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR; }
        if (quality != 5) { result.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes; }
        if (sharpening) { result.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DoSharpening; }
        result.InEnableOutputSubrects = false;
        return result;
    }
}
