#include "FrameGen/NeuralAdmission.h"
#include "FrameGen/NeuralRuntimeKernels.h"
#include "FrameGen/NeuralResearch.h"
#include "FrameGen/NeuralRuntimeFiles.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace TheosRenderPipeline::NeuralRendering;
static void Require(bool value, const char* why) { if (!value) { throw std::runtime_error(why); } }
static NeuralCapability Valid()
{
    NeuralCapability c;
    c.nvidia = c.adapterMatched = c.cudaReady = c.nvapiReady = c.universal = c.assetsVerified = c.budgetKnown = true;
    c.architecture = 61; c.smCount = 19; c.cudaDriver = 12090; c.driverVersion = 58278;
    c.dedicatedBytes = 2ull << 30; c.budgetBytes = 1536ull << 20; c.usageBytes = 64ull << 20;
    c.estimateBytes = EstimatePascalMemory(768, 432, 1);
    return c;
}
int wmain(int argc, wchar_t** argv)
{
    try {
        SetEnvironmentVariableW(L"TRP_NR_PASCAL_RESEARCH", L"2");
        SetEnvironmentVariableW(L"TRP_NR_PASCAL_PACK", L"D:/arbitrary-research-pack.trpk");
        const auto releaseConfig = ReadPascalConfiguration(L"missing-diagnostics.ini", L".");
        Require(!releaseConfig.mode && releaseConfig.pack.empty(), "release admission ignores research environment");
        SetEnvironmentVariableW(L"TRP_NR_PASCAL_RESEARCH", nullptr);
        SetEnvironmentVariableW(L"TRP_NR_PASCAL_PACK", nullptr);
        Require(!Valid().Refusal(), "valid 2 GiB adapter can admit without a VRAM floor");
        for (int sm = 1; sm <= 128; ++sm) { auto c = Valid(); c.smCount = sm; Require(!c.Refusal(), "all positive SM counts admitted"); }
        for (int arch : {0, 50, 60, 61, 62, 75, 86, 89, 120}) {
            for (bool edition : {false, true}) {
                for (unsigned driver : {0u, 58277u, 58278u, 58299u, 58999u, 59000u, 61088u}) {
                    for (bool asset : {false, true}) {
                        for (bool budget : {false, true}) {
                            auto c = Valid(); c.architecture = arch; c.universal = edition;
                            c.driverVersion = driver; c.assetsVerified = asset; c.budgetKnown = budget;
                            const bool expected = arch >= 75 || (arch == 61 && edition && driver >= 58278 && driver < 59000 && asset && budget);
                            Require((c.Refusal() == nullptr) == expected, "architecture/edition/driver/asset/budget matrix");
                        }
                    }
                }
            }
        }
        for (int field = 0; field != 7; ++field) {
            auto c = Valid();
            switch (field) {
            case 0: c.adapterMatched = false; break;
            case 1: c.cudaReady = false; break;
            case 2: c.nvapiReady = false; break;
            case 3: c.smCount = 0; break;
            case 4: c.cudaDriver = 0; break;
            case 5: c.cudaDriver = 12080; break;
            case 6: c.nvidia = false; break;
            }
            Require(c.Refusal(), "unknown adapter/component facts refuse");
        }
        auto c = Valid(); c.dedicatedBytes = 0;
        Require(!c.Refusal(), "zero or unreported dedicated VRAM does not override sufficient live budget");
        c = Valid(); c.cudaDriver = 13000; Require(!c.Refusal(), "newer CUDA driver API does not imply wrong compiler target");
        c.budgetBytes = c.usageBytes + c.estimateBytes; Require(!c.Refusal(), "exact headroom admits");
        --c.budgetBytes; Require(c.Refusal() == kPascalBudgetReason, "one byte short refuses");
        c.usageBytes = UINT64_MAX; Require(c.Refusal() == kPascalBudgetReason, "over-budget subtraction cannot wrap");
        c.retainedEstimateBytes = c.estimateBytes; Require(!c.Refusal(), "unchanged active resources are not charged twice");
        Require(!EstimatePascalMemory(0, 432, 1) && !EstimatePascalMemory(16385, 432, 1) && !EstimatePascalMemory(768, 432, 6), "invalid estimate inputs refuse");
        Require(EstimatePascalMemory(1130, 636, 2) > EstimatePascalMemory(768, 432, 1), "resolution/pass resource estimate grows");
        Require(std::string(kPascalStatus).find("experimental") != std::string::npos, "experimental status is explicit");

        // The live owner is charged until the safe boundary releases it. Use an
        // observed post-release usage, deliberately different from its estimate.
        {
            auto live = Valid(); live.budgetBytes = 4ull << 30;
            live.usageBytes = live.budgetBytes - (100ull << 20);
            live.estimateBytes = EstimatePascalMemory(1280,720,1);
            Require(live.Refusal() == kPascalBudgetReason, "replacement cannot fit alongside old owner");
            auto draft = live; draft.budgetPendingRetirement = true;
            Require(!draft.Refusal() && !draft.retainedEstimateBytes, "draft defers budget without claiming estimated residency credit");
            draft.assetsVerified = false;
            Require(draft.Refusal() == kPascalAssetReason, "deferred budget cannot bypass asset admission");
            bool owner = true, queried = false;
            Require(PrepareNeuralAdmission(true, [&] { owner = false; return true; }, [&] {
                Require(!owner, "live budget query follows old-owner release");
                queried = true;
                live.usageBytes = 2ull << 30; // newly observed DXGI usage
            }) && queried && !live.Refusal(), "reduced live-owner demand admits using fresh usage");
            queried = false;
            Require(!PrepareNeuralAdmission(true, [] { return false; }, [&] { queried = true; }) && !queried,
                "unsafe retirement never queries/admit a replacement");
            live.usageBytes = live.budgetBytes;
            PrepareNeuralAdmission(true, [] { return true; }, [] {});
            Require(live.Refusal() == kPascalBudgetReason, "actual unreclaimed budget still refuses after release");
        }

        const auto temp = std::filesystem::temp_directory_path() / (L"trp-admission-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(temp);
        const auto pack = temp / kPascalPackName;
        Require(!VerifyPascalPack(pack), "missing pack refuses");
        { std::ofstream output(pack, std::ios::binary); output.seekp(kPascalPackBytes - 1); output.put(0); }
        Require(!VerifyPascalPack(pack), "same-length unpinned pack refuses");
        if (argc > 1) {
            Require(VerifyPascalPack(argv[1]), "actual pinned pack and embedded runtime/launch manifest accepted");
            std::filesystem::copy_file(argv[1], pack, std::filesystem::copy_options::overwrite_existing);
            Require(VerifyPascalPack(pack), "copied pinned pack accepted");
            const auto timestamp = std::filesystem::last_write_time(pack);
            { std::fstream output(pack, std::ios::in | std::ios::out | std::ios::binary); output.seekg(600); char value{}; output.get(value); output.seekp(600); output.put(value ^ 1); }
            std::filesystem::last_write_time(pack, timestamp);
            Require(!VerifyPascalPack(pack), "tamper refuses even with original size/time");
            std::filesystem::resize_file(pack, kPascalPackBytes - 1);
            Require(!VerifyPascalPack(pack), "truncation refuses");
        }
        // Optional installed-package boundary: use the actual signed runtime and
        // pinned pack; the legacy/custom selection remains visible but refused.
        if (argc > 2) {
            using namespace TheosRenderPipeline::SourceDLSSG;
            const auto original = std::filesystem::path(argv[2]);
            Require(VerifyPascalAssets(original, true), "correctly installed original runtime and Pascal package accepted");
            for (const auto saved : {kDefaultNeuralRuntime, kInt8NeuralRuntime, std::string_view("custom.dll")}) {
                Require(NormalizeNeuralRuntimeSetting(std::string(saved), true) == saved, "saved explicit incompatible selection survives normalization");
                const auto selected = original.parent_path() / std::filesystem::path(saved).filename();
                Require(NeuralRuntimeChoiceReason(true, VerifyPascalAssets(selected)) != nullptr, "incompatible saved runtime greyed with reason");
            }
            Require(!NeuralRuntimeChoiceReason(true, VerifyPascalAssets(original)), "installed original 310.9.16 remains selectable for repair");
            const auto plugins = original.parent_path().parent_path().parent_path();
            const auto automatic = ResolveNeuralRuntimeSetting(kAutomaticNeuralRuntime, plugins,61,true);
            Require(automatic == original && !NeuralRuntimeChoiceReason(true,VerifyPascalAssets(automatic)), "Automatic repairs the saved legacy request using installed original runtime");
            const auto choices = ListNeuralRuntimes(plugins,"custom.dll");
            Require(std::find(choices.begin(),choices.end(),"custom.dll") != choices.end() && IsAutomaticNeuralRuntime(choices.front()),
                "incompatible custom selection remains visible beside Automatic");

        }
        Require(NeuralRuntimeChoiceReason(true,false) && !NeuralRuntimeChoiceReason(true,true) &&
            !NeuralRuntimeChoiceReason(false,false), "per-choice Pascal reasons preserve admitted repair and RTX choices");
        std::filesystem::remove(pack); std::filesystem::remove(temp);
        std::cout << "PASS admission matrix, unknown facts, budget boundaries and pack integrity\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
