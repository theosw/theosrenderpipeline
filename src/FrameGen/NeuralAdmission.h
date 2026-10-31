#pragma once

#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace TheosRenderPipeline::NeuralRendering
{
    inline constexpr std::string_view kPascalPackName = "nvngx_dlssnr_310.9.16_pascal_v1.trpk";
    inline constexpr std::string_view kPascalPackSha256 = "5AC8546C9E08AD8D0722DE54DA5E2BFDB1976FCCC2D227B3079E739BDA2C4B43";
    inline constexpr std::uint64_t kPascalPackBytes = 11'574'444;
    inline constexpr std::uint64_t kPascalGlobalsBytes = 62'811'622;
    inline constexpr const char* kPascalStatus = "GTX 10 compatibility kernels (experimental)";
    inline constexpr const char* kPascalEditionReason = "GTX 10 Neural Rendering needs the Universal edition. NR is off; normal rendering continues.";
    inline constexpr const char* kPascalDriverReason = "GTX 10 Neural Rendering needs NVIDIA driver 582.78 or a newer Pascal (R580) driver. NR is off; normal rendering continues.";
    inline constexpr const char* kPascalComponentsReason = "GTX 10 Neural Rendering could not verify CUDA/NVAPI or the rendering adapter. Reinstall a supported Pascal driver and restart Skyrim. NR is off; normal rendering continues.";
    inline constexpr const char* kPascalAssetReason = "GTX 10 NR compatibility pack or original NR 310.9.16 is missing or does not match. Reinstall Universal and its matching NR runtime. NR is off; normal rendering continues.";
    inline constexpr const char* kPascalBudgetUnknownReason = "GTX 10 NR could not query the rendering adapter's video-memory budget. NR is off; normal rendering continues.";
    inline constexpr const char* kPascalBudgetReason = "GTX 10 NR has insufficient video-memory budget for the estimated working set. Try one pass before upscaling or a lower resolution. NR is off; normal rendering continues.";

    // Facts refer to the actual rendering LUID, never a marketing name or SKU.
    // No runtime-failure latch belongs here; the recovery owner supplies that separately.
    struct NeuralCapability
    {
        LUID luid{};
        bool nvidia{}, adapterMatched{}, cudaReady{}, nvapiReady{}, universal{};
        int architecture{}, smCount{}, cudaDriver{};
        std::uint32_t driverVersion{}; // NVAPI hundredths; zero is unknown
        std::string driverBranch, driverSource;
        std::uint64_t dedicatedBytes{}, budgetBytes{}, usageBytes{}, estimateBytes{}, retainedEstimateBytes{};
        bool assetsVerified{}, budgetKnown{};
        // UI preflight only. The old owner is still charged by DXGI; replacement
        // budget admission must wait for proven retirement and a fresh query.
        bool budgetPendingRetirement{};
        bool Pascal() const { return nvidia && architecture == 61; }
        bool NvidiaFeatures() const { return nvidia && architecture >= 75; }
        const char* Refusal() const
        {
            if (!nvidia || !architecture) { return "NR hardware capability is unknown or unsupported. Check the rendering adapter and its driver."; }
            if (!Pascal()) { return NvidiaFeatures() ? nullptr : "Neural Rendering requires NVIDIA RTX hardware or experimental sm_61 hardware in Universal."; }
            if (!universal) { return kPascalEditionReason; }
            if (driverVersion < 58278 || driverVersion >= 59000) { return kPascalDriverReason; }
            if (!adapterMatched || !cudaReady || cudaDriver < 12090 || !nvapiReady || smCount <= 0) { return kPascalComponentsReason; }
            if (!assetsVerified) { return kPascalAssetReason; }
            if (!budgetKnown || !estimateBytes) { return kPascalBudgetUnknownReason; }
            const auto additional = estimateBytes > retainedEstimateBytes ? estimateBytes - retainedEstimateBytes : 0;
            if (!budgetPendingRetirement && additional && (usageBytes >= budgetBytes || additional > budgetBytes - usageBytes)) { return kPascalBudgetReason; }
            return nullptr;
        }
    };

    // Conservative admission estimate, not measured residency: declared globals,
    // 128 MiB fixed feature allowance per pass, 1024 B/model pixel for vendor
    // activations/history and 128 B/scene pixel for TRP preparation/resolve/history,
    // plus 64 MiB budget reserve. Use the largest selected model raster per pass.
    // Inputs are bounded before multiplication; zero means unknown/refuse.
    inline std::uint64_t EstimatePascalMemory(unsigned width, unsigned height, unsigned passes)
    {
        if (!width || !height || width > 16384 || height > 16384 || !passes || passes > 5) { return 0; }
        return kPascalGlobalsBytes + 64ull * 1024 * 1024 +
            passes * (128ull * 1024 * 1024 + std::uint64_t(width) * height * (1024 + 128));
    }

    // Run only at a boundary with no open list. A failed retirement must never
    // reach the fresh budget query or replacement allocation.
    template<class Retire, class Query>
    bool PrepareNeuralAdmission(bool replacement, Retire retire, Query query)
    {
        if (replacement && !retire()) { return false; }
        query();
        return true;
    }

    NeuralCapability QueryNeuralHardware(const LUID& luid, bool universal);
    // Fresh pack hash + manifest validation; actual admission can also rehash the runtime.
    bool VerifyPascalPack(const std::filesystem::path& pack);
    bool VerifyPascalAssets(const std::filesystem::path& runtime, bool freshRuntime = false);
    NeuralCapability QueryNeuralAdmission(const LUID& luid, const std::filesystem::path& runtime,
        unsigned width, unsigned height, unsigned passes, bool universal, bool log = false, std::uint64_t retainedEstimate = 0);
}
