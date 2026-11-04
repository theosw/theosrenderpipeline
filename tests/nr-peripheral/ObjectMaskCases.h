#pragma once
// Execute masks inside the actual NR recorder with a nonidentity feature double.
// This qualifies preparation/resolve/UI/state handling, not NVIDIA model quality.
static void ObjectMaskCases(GPU& gpu)
{
    using namespace TheosRenderPipeline::NeuralRendering;
    constexpr unsigned w=64,h=32,gw=32,gh=16;
    for(bool world:{false,true})for(int passes:{1,2})for(bool peripheral:{false,true})for(bool jittered:{false,true}) {
        const std::array<float,2> offset=jittered?std::array<float,2>{.75f,-.375f}:std::array<float,2>{};
        NeuralOptions options; options.enabled=true; options.beforeUpscaling=world; options.passes=passes;
        options.runtimePath="fixture.dll"; options.tuning.intensity=.8f; options.tuning.uiCorrection=!world;
        options.reconstruction.method=ResolveMethod::Ratio; options.reconstruction.inputScale=.75f;
        options.reconstruction.peripheralCompression=peripheral;
        options.extraPasses[0].linked=false; options.extraPasses[0].inputScale=.5f; options.extraPasses[0].preset=1;
        options.extraPasses[0].tuning.intensity=.6f; options.extraPasses[0].tuning.uiCorrection=!world;
        options.objectMasks={};
        options.objectMasks.passes[0].groups[0]={true,.25f,.75f};
        options.objectMasks.passes[1].groups[1]={true,.75f,.25f};
        options.tuning.useAutoSkinMask=true;options.extraPasses[0].tuning.useAutoSkinMask=true;
        Fixture::peripheral=peripheral; Fixture::worldOnly=world; Fixture::sourceWidth=w; Fixture::sourceHeight=h;
        Fixture::guideWidth=gw; Fixture::guideHeight=gh; Fixture::transform=true;
        Fixture::workWidth=ModelExtent(w,options.reconstruction); Fixture::workHeight=ModelExtent(h,options.reconstruction);
        auto second=options.reconstruction; second.inputScale=.5f;
        Fixture::secondWidth=ModelExtent(w,second); Fixture::secondHeight=ModelExtent(h,second);
        auto motion=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{}),DXGI_FORMAT_R32G32_FLOAT);
        auto depth=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{.4f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
        auto ui=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.05f,.025f,0,.25f})),composed=gpu.Texture(w,h);
        const std::vector<Pixel> pixels(w*h,{.2f,.3f,.4f,.5f});
        std::vector<Pixel> classes(gw*gh);
        for(unsigned y=0;y<gh;++y)for(unsigned x=0;x<gw;++x)
            classes[y*gw+x]={PackObjectPixel(static_cast<ObjectGroup>(x%4+1),y%3==0?.2f:.4f),0,0,0};
        auto mask=gpu.Texture(gw,gh,classes,DXGI_FORMAT_R32_FLOAT);
        std::vector<Pixel> baseline;
        // 0: no classification; 1: per-pass rules; 2: rules plus the classes preview.
        for(unsigned run:{0u,1u,2u}) {
            const bool masked=run!=0;
            options.objectMasks.previewGroups=run==2?0xFu:0u; options.objectMasks.previewClassesOnly=run==2;
            Fixture::creations=Fixture::evaluations=Fixture::resets=0;
            Fixture::objectControls=masked;Fixture::controlEvaluations=0;
            NeuralPass pass;pass.SetRateClock(&TestClock::Now);
            auto scene=gpu.Texture(w,h,pixels);
            gpu.Begin(); Require(pass.Record(gpu.device.Get(),gpu.list.Get(),0,options,true,true,float(gw),float(gh),
                motion.Get(),depth.Get(),world?nullptr:ui.Get(),scene.Get(),world?nullptr:composed.Get(),0,{},masked?mask.Get():nullptr,offset),pass.Status().c_str()); gpu.End();
            Require(Fixture::controlEvaluations==(masked?unsigned(passes):0),"every requested pass receives its own model-grid controls");
            const auto actual=gpu.Read(pass.Corrected());
            if(!masked){baseline=actual;continue;}
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
                const auto i=y*w+x; const auto qx=unsigned(std::clamp(int(std::floor((x+.5f)*gw/w+offset[0])),0,int(gw)-1)),
                    qy=unsigned(std::clamp(int(std::floor((y+.5f)*gh/h+offset[1])),0,int(gh)-1));
                // Rules with full strength leave the result; the preview tints classified pixels.
                const unsigned group=qy%3==0?0:qx%4+1;
                const std::array<std::array<float,3>,4> colors{{{.2f,.9f,.4f},{.25f,.55f,1},{1,.65f,.15f},{.8f,.3f,.95f}}};
                for(unsigned c=0;c<4;++c) {
                    const float expected=run==2&&c<3?(group?colors[group-1][c]:0.f):baseline[i][c];
                    Near(actual[i][c],expected,.000001,run==2?"classes preview on the final native-grid result":
                        "full-strength object rules leave the one/two-pass result");
                }
            }
            if(!world) {
                const auto real=gpu.Read(pass.Composed()),tag=gpu.Read(scene.Get());
                for(unsigned i=0;i<w*h;++i)for(unsigned c=0;c<4;++c) {
                    Near(tag[i][c],actual[i][c],0,"masked real frame and FG HUD-less input agree");
                    if(c<3)Near(real[i][c],actual[i][c]*.75f+std::array<float,3>{.05f,.025f,0}[c],.000001,"UI composes after object masks and preview");
                }
            }
        }
    }
    Fixture::secondWidth=Fixture::secondHeight=0;Fixture::transform=false;
    Fixture::objectControls=false;
    std::puts("PASS object rules and preview inside production NR recorder: both placements, one/two independent passes, scaled/peripheral grids, UI and FG tags");
}

static void PassObjectStrengthCases(GPU& gpu)
{
    using namespace TheosRenderPipeline::NeuralRendering;
    constexpr unsigned w=64,h=32,gw=32,gh=16;
    const std::vector<Pixel> pixels(w*h,{.25f,.5f,.75f,.625f});
    for (bool world:{false,true}) for (bool peripheral:{false,true}) for (bool resized:{false,true}) {
        NeuralOptions options; options.enabled=true; options.beforeUpscaling=world; options.passes=2;
        options.runtimePath="fixture.dll"; options.tuning.intensity=.8f;
        options.reconstruction.inputScale=peripheral?.75f:1; options.reconstruction.peripheralCompression=peripheral;
        options.extraPasses[0].linked=false; options.extraPasses[0].inputScale=resized?.5f:options.reconstruction.inputScale;
        options.extraPasses[0].tuning.intensity=.6f; options.extraPasses[0].preset=1;
        for (auto& pass:options.objectMasks.passes) { pass.groups[0].enabled=true; }
        Fixture::peripheral=peripheral; Fixture::worldOnly=world; Fixture::sourceWidth=w; Fixture::sourceHeight=h;
        Fixture::guideWidth=gw; Fixture::guideHeight=gh; Fixture::transform=true;
        Fixture::workWidth=ModelExtent(w,options.reconstruction); Fixture::workHeight=ModelExtent(h,options.reconstruction);
        auto reconstruction=options.reconstruction; reconstruction.inputScale=options.extraPasses[0].inputScale;
        Fixture::secondWidth=ModelExtent(w,reconstruction); Fixture::secondHeight=ModelExtent(h,reconstruction);
        auto motion=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{}),DXGI_FORMAT_R32G32_FLOAT);
        auto depth=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{.4f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
        auto mask=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{PackObjectPixel(ObjectGroup::FirstPerson,.4f),0,0,0}),DXGI_FORMAT_R32_FLOAT);
        auto occluded=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{PackObjectPixel(ObjectGroup::FirstPerson,.9f),0,0,0}),DXGI_FORMAT_R32_FLOAT);
        auto ui=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.05f,.025f,0,.25f})),composed=gpu.Texture(w,h);
        std::vector<Pixel> firstOnly;
        // One-pass reference, full chain, each exact bypass, fractional strengths,
        // then a full chain again (6), missing and occluded classification, and
        // disabled rules.
        for (unsigned scenario=0;scenario<10;++scenario) {
            const float s1=scenario>=7||scenario==3||scenario==4?0:scenario==5?.5f:1;
            const float s2=scenario>=7||scenario==2||scenario==4?0:scenario==5?.25f:1;
            options.passes=scenario==0?1:2;
            options.objectMasks.passes[0].groups[0].strength=s1;
            options.objectMasks.passes[1].groups[0].strength=s2;
            for (auto& rule:options.objectMasks.passes) { rule.groups[0].enabled=scenario!=9; }
            Fixture::creations=Fixture::evaluations=Fixture::resets=0;
            Fixture::objectControls=scenario!=7&&scenario!=9; Fixture::controlEvaluations=0;
            NeuralPass pass;pass.SetRateClock(&TestClock::Now);
            // Reuse the owner and all retired-slot descriptor tables across frames.
            std::vector<Pixel> actual;
            for (unsigned frame=0;frame<4;++frame) {
                auto scene=gpu.Texture(w,h,pixels);
                gpu.Begin(); Require(pass.Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,frame==0,true,
                    float(gw),float(gh),motion.Get(),depth.Get(),world?nullptr:ui.Get(),scene.Get(),world?nullptr:composed.Get(),
                    0,{},scenario==7?nullptr:scenario==8?occluded.Get():mask.Get(),{.75f,-.375f}),pass.Status().c_str()); gpu.End();
                actual=gpu.Read(pass.Corrected());
                for (unsigned i=0;i<w*h;++i) for (unsigned c=0;c<4;++c) {
                    const float original=pixels[i][c];
                    const float first=c<3?original+(scenario>=7?1:s1)*(original*.8f+.01f-original):original;
                    const float expected=scenario==0?first:
                        c<3?first+(scenario>=7?1:s2)*(first*.6f+.02f-first):original;
                    Near(actual[i][c],expected,.0004,"independent per-pass strengths blend the input of their own pass");
                    if(scenario==2) Near(actual[i][c],firstOnly[i][c],.000001,"zero second-pass strength retains the first pass including alpha");
                    if(scenario==4) Near(actual[i][c],original,.000001,"zero strength in both passes retains original values");
                }
                if(!world) {
                    const auto real=gpu.Read(pass.Composed()),tag=gpu.Read(scene.Get());
                    for(unsigned i=0;i<w*h;++i)for(unsigned c=0;c<4;++c) {
                        Near(tag[i][c],actual[i][c],0,"per-pass strength feeds the same real and FG scene");
                        if(c<3)Near(real[i][c],actual[i][c]*.75f+std::array<float,3>{.05f,.025f,0}[c],.000001,"native UI follows per-pass composition");
                    }
                }
            }
            if(scenario==0)firstOnly=actual;
            Require(Fixture::evaluations==unsigned(options.passes)*4,"strength zero still evaluates every requested NR pass");
        }
    }
    Fixture::secondWidth=Fixture::secondHeight=0; Fixture::transform=false; Fixture::objectControls=false;
    std::puts("PASS per-pass object strength endpoints, fractions, second-pass preservation, resized/peripheral grids and UI/FG agreement");
}
