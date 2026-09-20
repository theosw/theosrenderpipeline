#pragma once
#include <format>
static void CombinedPreparation(GPU& gpu){
    NeuralResolveKernels kernels;Check(kernels.Initialize(gpu.device.Get()),"fusion kernels");unsigned cases=0;
    for(auto format:{DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R8G8B8A8_UNORM})
    for(bool producer:{false,true})for(bool peripheral:{false,true}){
        const unsigned w=37,h=23,ww=17,wh=11,gw=19,gh=13;
        std::vector<Pixel> pixels(w*h);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
            pixels[y*w+x]={x%2?4.f:.01f,y%3?.14f:2.f,x%5?-.2f:.8f,.3f};
        auto source=gpu.Texture(w,h,pixels,format),encoded=gpu.Texture(w,h,{},format);
        auto separate=gpu.Texture(ww,wh,{},format),combined=gpu.Texture(ww,wh,{},format);
        ResolveConstants c;c.sourceWidth=w;c.sourceHeight=h;c.targetWidth=w;c.targetHeight=h;
        c.workWidth=ww;c.workHeight=wh;c.producerColor=producer;c.passthrough=0;c.peripheral=peripheral;
        c.guideWidth=gw;c.guideHeight=gh;c.motionScaleX=gw;c.motionScaleY=gh;
        c.encodedFormat=format==DXGI_FORMAT_R16G16B16A16_FLOAT?1:format==DXGI_FORMAT_R8G8B8A8_UNORM?2:0;
        gpu.Begin();Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),0,0,ResolveKernel::Ratio,c,source.Get(),nullptr,nullptr,encoded.Get()),"separate encode");
        c.targetWidth=ww;c.targetHeight=wh;
        Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),0,1,ResolveKernel::Downsample,c,encoded.Get(),nullptr,nullptr,separate.Get()),"separate downsample");
        Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),0,2,ResolveKernel::PrepareColor,c,source.Get(),nullptr,nullptr,combined.Get()),"fused encode/filter");gpu.End();
        auto a=gpu.Read(separate.Get()),b=gpu.Read(combined.Get());
        for(size_t i=0;i<a.size();++i)for(unsigned ch=0;ch<4;++ch)Near(b[i][ch],a[i][ch],format==DXGI_FORMAT_R8G8B8A8_UNORM?1.f/255.f:format==DXGI_FORMAT_R16G16B16A16_FLOAT?.0005:.000001,"fused colour matches separate quantized intermediate");
        // Depth and nonuniform/offscreen motion: compare fused and existing GPU paths.
        std::vector<Pixel> guides(gw*gh),vectors(gw*gh);
        for(unsigned y=0;y<gh;++y)for(unsigned x=0;x<gw;++x){guides[y*gw+x][0]=.1f+.8f*x/gw;vectors[y*gw+x]={-.3f+.7f*x/gw,.4f-.8f*y/gh,0,0};}
        auto depth=gpu.Texture(gw,gh,guides,DXGI_FORMAT_R32_FLOAT),motion=gpu.Texture(gw,gh,vectors,DXGI_FORMAT_R16G16_FLOAT);
        auto sd=gpu.Texture(ww,wh,{},DXGI_FORMAT_R32_FLOAT),sm=gpu.Texture(ww,wh,{},DXGI_FORMAT_R32G32_FLOAT);
        auto fd=gpu.Texture(ww,wh,{},DXGI_FORMAT_R32_FLOAT),fm=gpu.Texture(ww,wh,{},DXGI_FORMAT_R32G32_FLOAT);
        gpu.Begin();Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),0,0,ResolveKernel::PackDepth,c,depth.Get(),nullptr,nullptr,sd.Get()),"separate depth");
        Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),0,1,ResolveKernel::PackMotion,c,motion.Get(),nullptr,nullptr,sm.Get()),"separate motion");
        Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),0,2,ResolveKernel::PackGuides,c,depth.Get(),motion.Get(),nullptr,fd.Get(),fm.Get()),"fused guides");gpu.End();
        auto da=gpu.Read(sd.Get()),db=gpu.Read(fd.Get()),ma=gpu.Read(sm.Get()),mb=gpu.Read(fm.Get());
        for(size_t i=0;i<da.size();++i){Near(db[i][0],da[i][0],0,"fused depth exact");for(unsigned ch=0;ch<2;++ch)Near(mb[i][ch],ma[i][ch],0,"fused endpoint vectors exact");}
        ++cases;
    }
    std::printf("PASS %u fused preparation comparisons including nonlinear bright taps, FP16/UNORM8 storage and half-precision guides\n",cases);
}
static void PreparationRecorder(GPU& gpu){
    using namespace TheosRenderPipeline::NeuralRendering;unsigned cases=0;
    for(bool fusion:{false,true})for(bool peripheral:{false,true})
    for(unsigned route=0;route<4;++route)for(int passes:{1,2})for(float scale:{1.f,.5f}){
        const unsigned w=32,h=20,gw=16,gh=10;const bool world=route!=0,producer=route==2;
        const auto format=producer?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
        NeuralOptions options;options.enabled=true;options.runtimePath="scripted.dll";options.beforeUpscaling=route==1 || route==2;
        options.worldOnly=route>=2;options.passes=passes;options.tuning.uiCorrection=!world;
        auto& r=options.reconstruction;r.fusedPreparation=fusion;r.peripheralCompression=peripheral;r.inputScale=scale;r.producerColor=producer;
        if(passes==2){r.method=ResolveMethod::Ratio;r.colorIsHDR=!producer;}
        Fixture::peripheral=peripheral;Fixture::worldOnly=world;Fixture::sourceWidth=w;Fixture::sourceHeight=h;
        Fixture::guideWidth=gw;Fixture::guideHeight=gh;Fixture::workWidth=ModelExtent(w,r);Fixture::workHeight=ModelExtent(h,r);
        Fixture::creations=Fixture::evaluations=Fixture::resets=0;NeuralPass pass;NeuralHistory history;
        auto motion=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{}),DXGI_FORMAT_R16G16_FLOAT);
        auto depth=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{.5f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
        for(unsigned frame=0;frame<6;++frame){
            const Pixel pixel=producer?Pixel{4,-.2f,.5f,-.25f}:Pixel{.2f,.3f,.4f,.5f};
            auto scene=gpu.Texture(w,h,std::vector<Pixel>(w*h,pixel),format),ui=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.015f*frame,.02f,0,.25f}),format),composed=gpu.Texture(w,h,{},format);
            auto original=gpu.Read(scene.Get()),u=gpu.Read(ui.Get());
            // An eligibility break models loading; resume with fresh history.
            if(frame==3)history.ResetFor(options,false,false);
            const bool reset=history.ResetFor(options,true,false);
            gpu.Begin();Require(pass.Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,reset,true,float(gw),float(gh),
                motion.Get(),depth.Get(),world?nullptr:ui.Get(),scene.Get(),world?nullptr:composed.Get()),pass.Status().c_str());
            gpu.End();
            const bool colorFusion=fusion && EffectiveResolve(r)==ResolveMethod::Ratio &&
                (producer || r.colorIsHDR) && (scale<1 || peripheral);
            const auto expected=std::format("fusion requested={} colour={} guides={}",fusion,colorFusion,fusion && peripheral);
            Require(pass.Status().find(expected)!=std::string::npos,"actual preparation/no-op status");
            auto result=gpu.Read(pass.Corrected());
            for(size_t i=0;i<result.size();++i)for(unsigned ch=0;ch<4;++ch)Near(result[i][ch],original[i][ch],producer?.0005:1.f/255.f,"preparation identity including signed colour/current alpha");
            if(!world){auto real=gpu.Read(pass.Composed()),tag=gpu.Read(scene.Get());
                for(size_t i=0;i<real.size();++i){
                    for(unsigned ch=0;ch<3;++ch)Near(real[i][ch],result[i][ch]*(1-u[i][3])+u[i][ch],1.f/255.f,"current native UI after preparation");
                    for(unsigned ch=0;ch<4;++ch)Near(tag[i][ch],result[i][ch],0,"real/FG world agreement");}}
        }
        Require(Fixture::evaluations==6*unsigned(passes) && Fixture::resets==2*unsigned(passes),"both model histories reset and evaluate every source frame");
        auto changed=options;changed.reconstruction.fusedPreparation=!fusion;Require(pass.NeedsRecreation(changed,gw,gh),"retirement on fusion toggle");
        ++cases;
    }
    std::printf("PASS %u production preparation configurations, native/CS, placements, passes, scales, UI and loading resets\n",cases);
}
