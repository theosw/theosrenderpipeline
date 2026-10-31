#include "FrameGen/NeuralAdmission.h"
#include "FrameGen/NeuralRuntimeKernels.h"
#include "FrameGen/NeuralResearch.h"
#include "FrameGen/NeuralRuntimeFiles.h"
#include <fstream>
#include "FrameGen/NeuralRenderingRuntimeIdentity.h"
#include <chrono>
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

        // Exercise the same cache used by Present, with virtual time so a
        // 1,000,000-frame refusal/loading loop cannot hide periodic probes.
        {
            NeuralAdmissionCache cache;
            NeuralAdmissionRequest request{L"original.dll", 0, 0, 1, true, true, 0};
            auto now = NeuralAdmissionCache::Clock::now();
            unsigned queries{}, polls{};
            auto query = [&] { ++queries; auto value = Valid();
                value.extentPending = !request.width || !request.height;
                value.estimateBytes = EstimatePascalMemory(request.width, request.height, request.passes);
                return value; };
            auto poll = [&](NeuralCapability&) { ++polls; };
            const auto begin = NeuralAdmissionCache::Clock::now();
            for (unsigned i = 0; i < 1000000; ++i) {
                const auto& pending = cache.Get(request, now, query, poll);
                Require(pending.extentPending && !pending.Refusal(), "saved NR before world is pending, never refused");
            }
            const auto ns = std::chrono::duration<double,std::nano>(NeuralAdmissionCache::Clock::now()-begin).count()/1000000;
            Require(queries == 1 && polls == 0, "loading frames never repeat admission/probes");
            Require(ns < 10000, "unchanged admission cache costs less than 10 us per frame");
            now += std::chrono::hours(1);
            cache.Get(request, now, query, poll);
            Require(queries == 1 && polls == 0, "pending extent does not poll budget");
            request.width = 768; request.height = 432;
            Require(!cache.Get(request,now,query,poll).Refusal() && queries == 2, "first extent arrives and admits once");
            now += std::chrono::seconds(3);
            cache.Get(request,now,query,poll);
            Require(queries == 2 && polls == 1, "bounded budget poll does not reverify assets");
            request.beforeUpscaling = false; cache.Get(request,now,query,poll);
            request.runtime = L"other.dll"; cache.Get(request,now,query,poll);
            request.passes = 2; cache.Get(request,now,query,poll);
            request.universal = false; cache.Get(request,now,query,poll);
            Require(queries == 6, "placement runtime passes edition changes re-admit");
            cache.Reset();
            auto refused = [&] { ++queries; auto v = query(); v.assetsVerified = false; return v; };
            for (unsigned i=0;i<1000000;++i) { Require(cache.Get(request,now,refused,poll).Refusal()==kPascalAssetReason,"asset refusal stays cached"); }
            Require(queries==8 && polls==1,"refused frames never re-hash or probe");
            cache.Reset(); cache.Get(request,now,query,poll);
            Require(queries==9,"explicit Apply invalidates refusal");
            auto v=Valid(); v.estimateBytes=0;
            Require(v.Refusal()==kPascalEstimateReason,"missing estimate is not a budget-query failure");
            v.extentPending=true; Require(!v.Refusal(),"unknown extent is not a refusal");
            std::cout << "ADMISSION_CACHE pending_ns_per_frame=" << ns << " frames=1000000 queries=1 budget_polls=0\n";
        }

        const auto temp = std::filesystem::temp_directory_path() / (L"trp-admission-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(temp);
        {
            using namespace TheosRenderPipeline::NeuralRenderingRuntimeIdentity;
            const auto file=temp/"identity.bin";
            const auto replacement=temp/"replacement.bin";
            constexpr std::string_view abc="BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
            { std::ofstream out(file,std::ios::binary); out << "abc"; }
            Require(VerifyExpected(file,3,abc).matched,"startup hashes actual opened file");
            const auto hashes=HashInvocations();
            Require(VerifyExpected(file,3,abc).matched && hashes==HashInvocations(),"unchanged file identity reuses hash");
            const auto written=std::filesystem::last_write_time(file);
            { std::ofstream out(replacement,std::ios::binary); out << "bad"; }
            std::filesystem::last_write_time(replacement,written);
            std::filesystem::remove(file); std::filesystem::rename(replacement,file);
            Require(!VerifyExpected(file,3,abc).matched && hashes+1==HashInvocations(),"replacement with restored size/time rehashes by file ID");
            Require(!VerifyExpected(file,3,abc,true).matched && hashes+2==HashInvocations(),"explicit Apply forces hash of refused identity");
            std::filesystem::remove(file);
        }
        const auto pack = temp / kPascalPackName;
        Require(!VerifyPascalPack(pack), "missing pack refuses");
        { std::ofstream output(pack, std::ios::binary); output.seekp(kPascalPackBytes - 1); output.put(0); }
        Require(!VerifyPascalPack(pack), "same-length unpinned pack refuses");
        if (argc > 1) {
            Require(VerifyPascalPack(argv[1]), "actual pinned pack and embedded runtime/launch manifest accepted");
            std::filesystem::copy_file(argv[1], pack, std::filesystem::copy_options::overwrite_existing);
            Require(VerifyPascalPack(pack), "copied pinned pack accepted");
            const auto hashes = TheosRenderPipeline::NeuralRenderingRuntimeIdentity::HashInvocations();
            const auto probes = GetAdmissionMetrics().packProbes;
            for (int i=0; i<100; ++i) { Require(VerifyPascalPack(pack,false), "cached pinned pack accepted"); }
            Require(TheosRenderPipeline::NeuralRenderingRuntimeIdentity::HashInvocations()==hashes && GetAdmissionMetrics().packProbes==probes,
                "identity-stable file is neither rehashed nor reparsed");
            const auto timestamp = std::filesystem::last_write_time(pack);
            { std::fstream output(pack, std::ios::in | std::ios::out | std::ios::binary); output.seekg(600); char value{}; output.get(value); output.seekp(600); output.put(value ^ 1); }
            std::filesystem::last_write_time(pack, timestamp);
            Require(!VerifyPascalPack(pack,false), "file change identity detects tamper even with original size/time");
            Require(!VerifyPascalPack(pack,true), "explicit Apply rehash detects same size/time tamper");
            const auto refusedHashes=TheosRenderPipeline::NeuralRenderingRuntimeIdentity::HashInvocations();
            Require(!VerifyPascalPack(pack,false) && TheosRenderPipeline::NeuralRenderingRuntimeIdentity::HashInvocations()==refusedHashes,
                "same refused file identity does not rehash");
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
