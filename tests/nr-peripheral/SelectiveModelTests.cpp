#include "GPU.h"
#include "FeatureDouble.h"
#include "FrameGen/NeuralAdmission.h"

// A real production pass owns D3D resources/features while a deterministic DXGI
// policy record supplies pressure. Never infer reclaimed residency from its estimate.
static void LiveOwnerDemandReduction(GPU& gpu)
{
    using namespace TheosRenderPipeline::NeuralRendering;
    constexpr unsigned w=64,h=16;
    Fixture::peripheral=false; Fixture::worldOnly=true; Fixture::transform=true;
    Fixture::sourceWidth=Fixture::workWidth=Fixture::guideWidth=w;
    Fixture::sourceHeight=Fixture::workHeight=Fixture::guideHeight=h;
    Fixture::secondWidth=w; Fixture::secondHeight=h;
    Fixture::passWidth.fill(0); Fixture::passHeight.fill(0);
    Fixture::intensity.fill(.8f); Fixture::networkPreset.fill(-1);
    Fixture::creations=Fixture::evaluations=Fixture::resets=0;
    Fixture::lateCreationFrame=~0u; Fixture::createdFeatures.clear();
    auto scene=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.4f,.3f,.2f,1}));
    auto motion=gpu.Texture(w,h,std::vector<Pixel>(w*h,{}),DXGI_FORMAT_R32G32_FLOAT);
    auto depth=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.5f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
    NeuralOptions options; options.enabled=true; options.beforeUpscaling=true;
    options.passes=2; options.runtimePath="scripted.dll"; options.tuning.intensity=.8f;
    auto owner=std::make_unique<NeuralPass>();
    auto record=[&] {
        gpu.Begin();
        Require(owner->Record(gpu.device.Get(),gpu.list.Get(),0,options,true,true,float(w),float(h),
            motion.Get(),depth.Get(),nullptr,scene.Get(),nullptr),"record live demand-reduction owner");
        gpu.End();
    };
    Fixture::frame=0; record(); Fixture::frame=1; record();
    Require(owner->Corrected() && Fixture::creations==2,"two-pass live owner allocated before pressure");
    options.passes=1;
    Require(owner->NeedsRecreation(options,w,h),"pass reduction requires serial replacement");
    NeuralCapability admission;
    admission.nvidia=admission.adapterMatched=admission.cudaReady=admission.nvapiReady=admission.universal=admission.assetsVerified=admission.budgetKnown=true;
    admission.architecture=61; admission.smCount=19; admission.cudaDriver=12090; admission.driverVersion=58278;
    admission.budgetBytes=4ull<<30; admission.usageBytes=admission.budgetBytes-(100ull<<20);
    admission.estimateBytes=EstimatePascalMemory(1280,720,1);
    Require(admission.Refusal()==kPascalBudgetReason,"replacement refuses while old live owner is charged");
    bool queried=false;
    Require(PrepareNeuralAdmission(true,[&] {
        Require(gpu.fence->GetCompletedValue()==gpu.serial,"safe GPU boundary precedes owner release");
        if(!owner->ReleaseAfterRetirement()) return false;
        owner.reset(); return true;
    },[&] {
        Require(!owner,"fresh budget query cannot run alongside old live owner");
        queried=true;
        admission.usageBytes=2ull<<30; // observed-policy fixture, not measured residency
    }) && queried && !admission.Refusal() && !admission.retainedEstimateBytes,
        "fresh post-release budget admits reduced demand with zero speculative credit");
    const auto oldCreations=Fixture::creations;
    Fixture::creations=0; Fixture::createdFeatures.clear(); // a new owner's pass indices start at zero
    owner=std::make_unique<NeuralPass>(); Fixture::frame=2; record();
    Require(oldCreations==2 && Fixture::creations==1 && owner->Corrected(),"admitted replacement creates and records exactly one pass");
    Require(owner->ReleaseAfterRetirement(),"retire replacement owner"); owner.reset();
    std::puts("PASS live two-pass owner -> retirement/release -> fresh budget -> one-pass replacement");
}

int main()
{
    using namespace TheosRenderPipeline::NeuralRendering;
    GPU gpu;
    LiveOwnerDemandReduction(gpu);
    for (int route : {0, 1, 2}) for (int passes : {1, 2, 5}) for (int mode : {0, 1, 2, 3}) {
        constexpr unsigned w=64, h=16;
        const bool before=route==0, world=route!=2;
        Fixture::peripheral=false; Fixture::worldOnly=world; Fixture::transform=true;
        Fixture::sourceWidth=Fixture::workWidth=Fixture::guideWidth=w;
        Fixture::sourceHeight=Fixture::workHeight=Fixture::guideHeight=h;
        Fixture::secondWidth=w; Fixture::secondHeight=h;
        Fixture::passWidth.fill(0); Fixture::passHeight.fill(0);
        Fixture::intensity.fill(.8f); Fixture::networkPreset.fill(-1);
        Fixture::creations=Fixture::evaluations=Fixture::resets=0;
        Fixture::lateCreationFrame=~0u; Fixture::createdFeatures.clear();
        NeuralOptions options; options.enabled=true; options.beforeUpscaling=before; options.worldOnly=world;
        options.passes=passes; options.runtimePath="scripted.dll"; options.tuning.intensity=.8f; options.tuning.uiCorrection=!world;
        for (auto& extra:options.extraPasses) { extra.linked=false; extra.inputScale=1; extra.preset=0; extra.tuning=options.tuning; }
        const unsigned changed=passes==5?3:passes-1;
        const float extraScale=mode==1 && passes>1 ? .75f : 1.f;
        if(passes>1) {
            options.extraPasses[changed-1].inputScale=extraScale;
            Fixture::passWidth[changed]=unsigned(w*extraScale); Fixture::passHeight[changed]=unsigned(h*extraScale);
        }
        Fixture::objectControls=mode==2;
        if(mode==2) for(auto& rule:options.objectMasks.passes) { rule.groups[0].enabled=true; rule.groups[0].strength=.5f; }
        if(mode==3) options.reconstruction.modelRate=ModelRate::EveryOtherFrame;
        auto scene=gpu.Texture(w,h), original=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.4f,.3f,.2f,1}));
        auto motion=gpu.Texture(w,h,std::vector<Pixel>(w*h,{0,0,0,0}),DXGI_FORMAT_R32G32_FLOAT);
        auto depth=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.5f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
        auto mask=gpu.Texture(w,h,std::vector<Pixel>(w*h,{PackObjectPixel(ObjectGroup::FirstPerson,.5f),0,0,0}),DXGI_FORMAT_R32_FLOAT);
        auto ui=gpu.Texture(w,h,std::vector<Pixel>(w*h,{0,0,0,0})), composed=gpu.Texture(w,h);
        NeuralPass pass; unsigned frame=0;
        auto record=[&] {
            Fixture::frame=frame++; Fixture::evaluatedFeatures.clear();
            gpu.Begin(); Check(Interop::RecordCopy(gpu.list.Get(),original.Get(),scene.Get()),"fresh input");
            Require(pass.Record(gpu.device.Get(),gpu.list.Get(),0,options,Fixture::frame==0,true,float(w),float(h),motion.Get(),depth.Get(),
                world?nullptr:ui.Get(),scene.Get(),world?nullptr:composed.Get(),0,{},mode==2?mask.Get():nullptr),"production selective record");
            gpu.End();
        };
        for (unsigned f=0; f<6; ++f) record();
        Require(Fixture::creations==unsigned(passes),"initial feature count");
        const auto baseline=gpu.Read(pass.Corrected());
        if (route == 1) {
            auto draft = options; draft.worldOnly = false; // saved/base configuration
            draft = NormalizeNeuralRoute(draft, true);
            Require(!pass.NeedsRecreation(draft,w,h), "unchanged CS after-upscaling draft retains allocation credit");
            auto budget = TheosRenderPipeline::NeuralRendering::NeuralCapability{};
            budget.nvidia=budget.adapterMatched=budget.cudaReady=budget.nvapiReady=budget.universal=budget.assetsVerified=budget.budgetKnown=true;
            budget.architecture=61; budget.smCount=19; budget.cudaDriver=12090; budget.driverVersion=58278;
            budget.budgetBytes=budget.usageBytes=4ull<<30;
            budget.estimateBytes=EstimatePascalMemory(w,h,passes);
            budget.retainedEstimateBytes=!pass.NeedsRecreation(draft,w,h)?budget.estimateBytes:0;
            Require(!budget.Refusal(), "unchanged CS draft uses retained credit even with no free headroom");
        }

        // On a five-pass chain replace Pass 4, with Pass 5 retained downstream.
        if(changed==0) options.reconstruction.preset=1; else options.extraPasses[changed-1].preset=1;
        Fixture::networkPreset[changed]=1;
        Require(pass.NeedsRecreation(options,w,h) && !pass.NeedsResourceRecreation(options,w,h),"model edit is selective");
        auto* output=pass.Corrected();
        Require(pass.RetireChangedModels(options),"retired model replacement"); // gpu.End proved retirement.
        Require(pass.Corrected()==output,"chain output allocation retained");
        for(unsigned f=0;f<3;++f) record();
        Require(Fixture::creations==unsigned(passes+1) && Fixture::createdFeatures.back()==changed,"only edited feature recreated");
        auto pixels=gpu.Read(pass.Corrected());
        // Sparse change storage is FP16, as in ThirdFrameTests.
        for(unsigned p=0;p<pixels.size();++p) for(unsigned c=0;c<3;++c) Near(pixels[p][c],baseline[p][c],.0005,"unchanged affine model pixels");
        // A second replacement checks exact reset propagation on its first run.
        if(changed==0) options.reconstruction.preset=0; else options.extraPasses[changed-1].preset=0;
        Fixture::networkPreset[changed]=-1;
        Require(pass.RetireChangedModels(options),"second retirement");
        record(); if(changed>0) record();
        bool sawChanged=false;
        for(const auto [index,reset]:Fixture::evaluatedFeatures) {
            if(index>=changed) { Require(reset,"changed/downstream history reset"); sawChanged|=index==changed; }
            else Require(!reset,"upstream history preserved");
        }
        Require(sawChanged,"replacement reaches evaluation after ramp");
        if(passes==5) {
            record(); bool sawDownstream=false;
            for(const auto [index,reset]:Fixture::evaluatedFeatures) {
                Require(reset==(index==4),"retained Pass 5 resets when it rejoins the prefix");
                sawDownstream|=index==4;
            }
            Require(sawDownstream,"downstream retained feature rejoins");
        }
        if(passes>1) {
            const auto old=options.extraPasses[0].inputScale;
            options.extraPasses[0].inputScale=.5f; Require(pass.NeedsResourceRecreation(options,w,h),"resolution edit retains broad rebuild");
            options.extraPasses[0].inputScale=old;
        }
        options.beforeUpscaling=!options.beforeUpscaling;
        Require(pass.NeedsResourceRecreation(options,w,h),"placement edit retains broad rebuild");
        options.beforeUpscaling=before;
        if(changed==0) options.reconstruction.preset=1; else options.extraPasses[changed-1].preset=1;
        if(changed>0) {
            Require(pass.RetireChangedModels(options),"retire before scripted creation refusal");
            const auto creations=Fixture::creations;
            Fixture::refuseIndex=creations;
            Fixture::frame=frame++;
            gpu.Begin();
            Require(!pass.Record(gpu.device.Get(),gpu.list.Get(),0,options,false,true,float(w),float(h),motion.Get(),depth.Get(),
                world?nullptr:ui.Get(),scene.Get(),world?nullptr:composed.Get()), "replacement refusal propagates to session recovery");
            gpu.Discard();
            Require(Fixture::creations==creations,"no duplicate feature on refused replacement");
            Fixture::refuseIndex=~0u;
            options.extraPasses[changed-1].preset=0;
        }
        Fixture::refuseRetirement=true;
        Require(!pass.RetireChangedModels(options),"unconfirmed creation retirement refuses replacement");
        Fixture::refuseRetirement=false;
    }
    std::puts("PASS: selective model replacement, preserved owners/resources, prefix ramp, downstream resets, 1/2/5 passes and three routes");
}
