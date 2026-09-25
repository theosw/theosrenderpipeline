#include "GPU.h"
#include "FeatureDouble.h"
#include "PreparationTests.h"

// Independent double-precision reference: monotone inverse is found by
// bisection, rather than copying the shader's analytic inverse.
static double Pack(double p,double n,double w){
    const double r=2*p/n-1,a=std::abs(r);double mapped=a;
    if(a>1)mapped=.9+(a-1)*.25;
    else if(a>.8)mapped=.8+.1*(a-.8)/(.1+.5*(a-.8));
    return (std::copysign(mapped,r)/.9+1)*w*.5;
}
static double Unpack(double p,double n,double w){
    double lo=-10*n,hi=11*n;
    for(int i=0;i<70;++i){double mid=(lo+hi)*.5;if(Pack(mid,n,w)<p)lo=mid;else hi=mid;}
    return (lo+hi)*.5;
}
static std::vector<Pixel> Pattern(unsigned w,unsigned h,bool producer=false){
    std::vector<Pixel> v(size_t(w)*h);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        v[y*w+x]={.1f+.6f*x/w,.1f+.6f*y/h,.2f+.3f*((x+y)%13)/12,.2f+.6f*((x+y)%5)/4};
        if(producer){v[y*w+x][0]*=50;v[y*w+x][1]=x%3?-2.f:3.f;v[y*w+x][3]=-.5f;}
    }
    return v;
}
static void Kernels(GPU& gpu){
    NeuralResolveKernels kernels;Check(kernels.Initialize(gpu.device.Get()),"production kernels");
    for(const auto dims:{std::array<unsigned,6>{200,100,180,90,100,50},{203,117,91,53,97,57}}){
        const auto [w,h,ww,wh,gw,gh]=dims;
        ResolveConstants c;c.sourceWidth=w;c.sourceHeight=h;c.targetWidth=c.workWidth=ww;c.targetHeight=c.workHeight=wh;
        c.peripheral=1;c.guideWidth=gw;c.guideHeight=gh;c.motionScaleX=float(gw);c.motionScaleY=float(gh);
        auto source=Pattern(w,h);auto a=gpu.Texture(w,h,source);auto packed=gpu.Texture(ww,wh);
        auto pd=gpu.Texture(ww,wh,{},DXGI_FORMAT_R32_FLOAT);auto pm=gpu.Texture(ww,wh,{},DXGI_FORMAT_R32G32_FLOAT);
        std::vector<Pixel> guides(size_t(gw)*gh);
        for(unsigned y=0;y<gh;++y)for(unsigned x=0;x<gw;++x)guides[y*gw+x][0]=float(y*gw+x);
        auto depth=gpu.Texture(gw,gh,guides,DXGI_FORMAT_R32_FLOAT);
        for(auto mv:{std::array<float,2>{0,0},{.08f,-.07f},{-.4f,.6f}}){
            auto motion=gpu.Texture(gw,gh,std::vector<Pixel>(size_t(gw)*gh,Pixel{mv[0],mv[1],0,0}),DXGI_FORMAT_R32G32_FLOAT);
            gpu.Begin();
            Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),0,1,ResolveKernel::Downsample,c,a.Get(),nullptr,nullptr,packed.Get()),"pack colour");
            Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),0,4,ResolveKernel::PackDepth,c,depth.Get(),nullptr,nullptr,pd.Get()),"pack depth");
            Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),0,5,ResolveKernel::PackMotion,c,motion.Get(),nullptr,nullptr,pm.Get()),"pack motion");gpu.End();
            const auto d=gpu.Read(pd.Get()),m=gpu.Read(pm.Get()),color=gpu.Read(packed.Get());
            for(unsigned y=0;y<wh;++y)for(unsigned x=0;x<ww;++x){
                const double nx=Unpack(x+.5,w,ww),ny=Unpack(y+.5,h,wh);
                const auto gx=std::clamp(unsigned(nx*gw/w),0u,gw-1),gy=std::clamp(unsigned(ny*gh/h),0u,gh-1);
                // At an exact integer sampling boundary, CPU/GPU float rounding
                // can select either adjacent point. Check non-boundary probes.
                if(std::abs(nx*gw/w-std::round(nx*gw/w))>1e-4 && std::abs(ny*gh/h-std::round(ny*gh/h))>1e-4)
                    Near(d[y*ww+x][0],gy*gw+gx,0,"depth follows warped position");
                Near(m[y*ww+x][0],Pack(nx+double(mv[0])*w,w,ww)-Pack(nx,w,ww),.0002,"X endpoint motion including offscreen");
                Near(m[y*ww+x][1],Pack(ny+double(mv[1])*h,h,wh)-Pack(ny,h,wh),.0002,"Y endpoint motion including offscreen");
                if(w==200 && x>15 && x<ww-15 && y>10 && y<wh-10)
                    for(unsigned ch=0;ch<4;++ch)Near(color[y*ww+x][ch],source[(y+5)*w+x+10][ch],.00004,"centre retains source density and alpha");
            }
        }
        // A nonzero constant model residual must map back uniformly, including
        // the nonlinear edges, while preserving the original full-resolution alpha.
        auto altered=gpu.Read(packed.Get());for(auto& p:altered){p[0]+=.03f;p[1]-=.02f;p[2]+=.04f;}
        auto model=gpu.Texture(ww,wh,altered),scratch=gpu.Texture(w,wh),output=gpu.Texture(w,h);
        gpu.Begin();c.mode=0;
        Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),1,2,ResolveKernel::Residual,c,packed.Get(),model.Get(),nullptr,scratch.Get()),"horizontal resolve");
        c.mode=1;Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),1,3,ResolveKernel::Residual,c,a.Get(),scratch.Get(),nullptr,output.Get()),"vertical resolve");gpu.End();
        const auto actual=gpu.Read(output.Get());
        for(size_t i=0;i<actual.size();++i){
            for(unsigned ch=0;ch<3;++ch)Near(actual[i][ch],source[i][ch]+std::array<float,3>{.03f,-.02f,.04f}[ch],.0002,"nonzero residual restores across full image");
            Near(actual[i][3],source[i][3],0,"native alpha unchanged");
        }
    }
    std::puts("PASS: production D3D12 pack/resolve, mixed guide extents, rounded work extents, centre density, offscreen endpoint motion, depth and alpha");
}
static void Passes(GPU& gpu){
    using namespace TheosRenderPipeline::NeuralRendering;
    unsigned cases=0;
    for(bool peripheral:{false,true})for(int route:{0,1,2,3})for(int passes:{1,2})for(float scale:{1.f,.5f}){
        // 0=native late/UI, 1=native early, 2=CS early/producer RGB, 3=CS late.
        const bool world=route!=0,producer=route==2;
        const unsigned w=100,h=60,gw=50,gh=30;
        NeuralOptions options;options.enabled=true;options.runtimePath=std::filesystem::absolute("scripted.dll");
        options.beforeUpscaling=route==1 || route==2;options.worldOnly=route>=2;options.passes=passes;
        options.tuning.uiCorrection=!world;options.reconstruction.peripheralCompression=peripheral;
        options.reconstruction.producerColor=producer;options.reconstruction.inputScale=scale;
        const auto ww=ModelExtent(w,options.reconstruction),wh=ModelExtent(h,options.reconstruction);
        Fixture::peripheral=peripheral;Fixture::worldOnly=world;Fixture::sourceWidth=w;Fixture::sourceHeight=h;
        Fixture::guideWidth=gw;Fixture::guideHeight=gh;Fixture::workWidth=ww;Fixture::workHeight=wh;
        Fixture::creations=Fixture::evaluations=Fixture::resets=0;
        auto depth=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{.4f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
        auto motion=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{}),DXGI_FORMAT_R32G32_FLOAT);
        auto uiPixels=std::vector<Pixel>(w*h,{.05f,.025f,0,.25f});
        auto ui=gpu.Texture(w,h,uiPixels),composed=gpu.Texture(w,h);
        NeuralPass pass;
        Require(pass.RetainedRuntimeBuild(options.runtimePath)==RuntimeBuild::Unknown,"uninitialized pass has no verified module");
        for(unsigned frame=0;frame<4;++frame){
            auto pixels=Pattern(w,h,producer);for(auto& p:pixels)p[2]+=.01f*frame;
            auto scene=gpu.Texture(w,h,pixels);
            gpu.Begin();
            const bool recorded=pass.Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,frame==0,true,float(gw),float(gh),
                motion.Get(),depth.Get(),world?nullptr:ui.Get(),scene.Get(),world?nullptr:composed.Get());
            Require(recorded,pass.Status().c_str());gpu.End();
            Require(!pass.NeedsRecreation(options,gw,gh),"stable producer extents do not recreate packed feature");
            Require(pass.RetainedRuntimeBuild(options.runtimePath)==RuntimeBuild::Nexus3108,"initialized pass retains the exact verified request");
            Require(pass.RetainedRuntimeBuild(options.runtimePath.parent_path()/"other.dll")==RuntimeBuild::Unknown,"changed runtime cannot borrow the live identity");
            Require(pass.RetainedRuntimeBuild("scripted.dll")==RuntimeBuild::Unknown,"relative runtime requests still require path verification");
            auto disabled=options;disabled.enabled=false;
            Require(!pass.NeedsRecreation(disabled,gw,gh),"off/on retains initialized feature resources");
            auto changed=options;changed.reconstruction.peripheralCompression=!peripheral;
            Require(pass.NeedsRecreation(changed,gw,gh),"layout toggle requires retirement/recreation");
            Require(pass.NeedsRecreation(options,gw+1,gh),"producer guide resize requires recreation");
            auto result=gpu.Read(pass.Corrected());
            for(size_t i=0;i<result.size();++i)for(unsigned ch=0;ch<4;++ch)
                Near(result[i][ch],pixels[i][ch],.0002,"identity model preserves original including signed producer colour");
            if(!world){
                auto output=gpu.Read(pass.Composed());auto hudless=gpu.Read(scene.Get());
                for(size_t i=0;i<output.size();++i){
                    for(unsigned ch=0;ch<3;++ch)Near(output[i][ch],pixels[i][ch]*.75f+uiPixels[i][ch],.0002,"untouched native UI composited once");
                    Near(output[i][3],std::max(pixels[i][3],.25f),.0002,"native UI alpha");
                    for(unsigned ch=0;ch<4;++ch)Near(hudless[i][ch],result[i][ch],0,"real frame and FG HUDless agree");
                }
            }
        }
        Require(Fixture::creations==unsigned(passes) && Fixture::evaluations==unsigned(passes)*4 && Fixture::resets==unsigned(passes),
            "distinct temporal features evaluate once per source frame without redundant resets");++cases;
    }
    std::printf("PASS: %u full production-pass cases, scripted identity NR, off/on, native early/late and CS producer, one/two passes, repeated retired slots\n",cases);
}
static void IndependentPasses(GPU& gpu){
    using namespace TheosRenderPipeline::NeuralRendering;
    unsigned cases=0;
    for(bool peripheral:{false,true})for(int route:{0,1,2,3})for(float secondScale:{.25f,.65f,1.f})for(bool transform:{false,true}){
        if(transform && route>=2)continue;
        const bool world=route!=0,producer=route==2;
        const unsigned w=103,h=61,gw=51,gh=30;
        NeuralOptions options;options.enabled=true;options.runtimePath="scripted.dll";
        options.beforeUpscaling=route==1 || route==2;options.worldOnly=route>=2;options.passes=2;
        options.tuning.uiCorrection=!world;options.reconstruction.peripheralCompression=peripheral;
        options.reconstruction.producerColor=producer;options.reconstruction.inputScale=.65f;
        options.secondPass.linked=false;options.secondPass.inputScale=secondScale;
        options.secondPass.preset=1;options.secondPass.tuning.uiCorrection=!world;
        if(transform){options.tuning.intensity=.8f;options.secondPass.tuning.intensity=.6f;}
        Fixture::peripheral=peripheral;Fixture::worldOnly=world;Fixture::sourceWidth=w;Fixture::sourceHeight=h;
        Fixture::guideWidth=gw;Fixture::guideHeight=gh;
        Fixture::workWidth=ModelExtent(w,options.reconstruction);Fixture::workHeight=ModelExtent(h,options.reconstruction);
        auto second=options.reconstruction;second.inputScale=secondScale;
        Fixture::secondWidth=ModelExtent(w,second);Fixture::secondHeight=ModelExtent(h,second);Fixture::transform=transform;
        Fixture::creations=Fixture::evaluations=Fixture::resets=0;
        auto motion=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{}),DXGI_FORMAT_R32G32_FLOAT);
        auto depth=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{.4f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
        auto ui=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.05f,.025f,0,.25f})),composed=gpu.Texture(w,h);
        NeuralPass pass;NeuralHistory history;
        for(unsigned frame=0;frame<4;++frame){
            auto pixels=transform?std::vector<Pixel>(w*h,{.2f,.3f,.4f,.5f}):Pattern(w,h,producer);
            auto scene=gpu.Texture(w,h,pixels);
            if(frame==2)history.Invalidate();
            gpu.Begin();Require(pass.Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,history.ResetFor(options,true,false),true,float(gw),float(gh),
                motion.Get(),depth.Get(),world?nullptr:ui.Get(),scene.Get(),world?nullptr:composed.Get()),pass.Status().c_str());gpu.End();
            auto actual=gpu.Read(pass.Corrected());
            for(size_t i=0;i<actual.size();++i)for(unsigned ch=0;ch<4;++ch){
                const auto expected=transform && ch<3?(pixels[i][ch]*.8f+.01f)*.6f+.02f:pixels[i][ch];
                Near(actual[i][ch],expected,.0004,"custom pass 2 chains nonidentity output or preserves all original detail with identity");
            }
            if(!world){auto real=gpu.Read(pass.Composed()),tag=gpu.Read(scene.Get());
                for(size_t i=0;i<actual.size();++i)for(unsigned ch=0;ch<4;++ch){
                    Near(tag[i][ch],actual[i][ch],0,"custom passes feed same real and FG scene");
                    if(ch<3)Near(real[i][ch],actual[i][ch]*.75f+std::array<float,3>{.05f,.025f,0}[ch],.0002,"native UI after both passes");
                }
            }
        }
        Require(Fixture::creations==2 && Fixture::evaluations==8 && Fixture::resets==4,"independent histories reset after eligibility break");
        Require(!pass.NeedsRecreation(options,gw,gh),"custom settings stable");
        auto changed=options;changed.secondPass.tuning.intensity=.1f;
        Require(!pass.NeedsRecreation(changed,gw,gh),"tuning resets history without recreating feature");
        Require(history.ResetFor(changed,true,false),"second tuning change resets histories");
        changed=options;changed.secondPass.preset=0;Require(pass.NeedsRecreation(changed,gw,gh),"second preset requires retirement");
        changed=options;changed.secondPass.inputScale=.8f;Require(pass.NeedsRecreation(changed,gw,gh),"second resolution requires retirement");
        ++cases;
    }
    Fixture::secondWidth=Fixture::secondHeight=0;Fixture::transform=false;
    std::printf("PASS: %u independent pass configurations; rounded sizes, larger/smaller/equal second grid, per-pass tuning/preset, identity detail, chained transform and UI/history contracts\n",cases);
}
int main(){GPU gpu;Kernels(gpu);Passes(gpu);CombinedPreparation(gpu);PreparationRecorder(gpu);IndependentPasses(gpu);}
