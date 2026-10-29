#pragma once
namespace TheosRenderPipeline::SourceDLSSG
{
// Area downsampling, separable Lanczos3 residual and luma/OkLab ratio transfer.
// Ratio transfer, hue correction and AP1 clipping correspond to RenoDX's public
// UpgradeToneMap, HueOKLab and clamp::AP1 components (MIT).
// Copyright (c) 2025 Carlos Lopez Jr. See THIRD-PARTY.md for the full notice,
// pinned upstream links and component scope; OkLab is defined by Bjorn Ottosson.
// Peripheral radius mapping adapted from optimizer-fps-dlss5, revision 71f5cfa,
// Copyright (c) 2026 Yuri Grib (BeliyG3), MIT. See THIRD-PARTY.md.
inline constexpr char kNeuralResolveShader[] = R"(
cbuffer NRParams : register(b0) {
    uint SourceWidth, SourceHeight, TargetWidth, TargetHeight;
    uint SourceIsBgra, Mode, Passthrough, ProducerColor;
    uint WorkWidth, WorkHeight;
    float TransferStrength, ColourStrength, MaxRatio, WhitePoint;
    uint Peripheral, EncodedFormat;
    uint GuideWidth, GuideHeight;
    float MotionScaleX, MotionScaleY;
    uint SplitX;
    float ExposureBias, ExposureRate;
    uint AdaptiveExposure, ExposureReset;
    // Before upscaling: the frame's camera jitter in source pixels. A steady-grid
    // position u was rendered at u + Jitter. Zero for every other dispatch.
    float JitterX, JitterY;
    // Broad and Detail: scale NR's region-wide relighting and its local detail.
    float Broad, Detail;
    uint ToneBands;
    // Model rate: camera jitter now and on the previous frame (render pixels),
    // whether only the guides are jittered (after upscaling), depth direction,
    // whether both frames classified objects, and the stored change's floor.
    float CameraJitterX, CameraJitterY, PreviousJitterX, PreviousJitterY;
    uint GuidesJittered, DepthInverted, Categories;
    float ChangeFloor;
};
Texture2D<float4> Tex0 : register(t0);
Texture2D<float4> Tex1 : register(t1);
Texture2D<float4> Tex2 : register(t2);
// Adaptive exposure: the white point MeasureExposure wrote this frame.
Texture2D<float> ExposureTex : register(t3);
// Broad and Detail: NR's brightness change in stops after the edge-aware blur.
Texture2D<float2> ToneTex : register(t4);
RWTexture2D<float4> OutputColor : register(u0);
// Only the entry point's output is live in the compiled shader.
RWTexture2D<float> OutputDepth : register(u0);
RWTexture2D<float2> OutputMotion : register(u0);
RWTexture2D<float2> OutputSecondMotion : register(u1);
// Symmetric 80/90 map. The central band has unit density before global input
// scaling; its first derivative joins continuously to the compressed edges.
// Linear extension preserves offscreen motion instead of clipping endpoints.
float PackRadius(float r) {
    r=abs(r);
    if (r<=0.8) return r;
    if (r>1) return 0.9+(r-1)*0.25;
    float t=(r-0.8)/0.2;
    return 0.8+0.1*t/(0.5+0.5*t);
}
float UnpackRadius(float r) {
    r=abs(r);
    if (r<=0.8) return r;
    if (r>0.9) return 1+(r-0.9)/0.25;
    float y=(r-0.8)/0.1;
    return 0.8+0.2*(0.5*y/(1-0.5*y));
}
float PackAxis(float p, float source, float work) {
    if (!Peripheral) return p*work/source;
    float r=2*p/source-1;
    return (sign(r)*PackRadius(r)/0.9+1)*0.5*work;
}
float UnpackAxis(float p, float source, float work) {
    if (!Peripheral) return p*source/work;
    float r=(2*p/work-1)*0.9;
    return (sign(r)*UnpackRadius(r)+1)*0.5*source;
}
float2 PackPosition(float2 p) {
    return float2(PackAxis(p.x,SourceWidth,WorkWidth),PackAxis(p.y,SourceHeight,WorkHeight));
}
float2 UnpackPosition(float2 p) {
    return float2(UnpackAxis(p.x,SourceWidth,WorkWidth),UnpackAxis(p.y,SourceHeight,WorkHeight));
}
float3 Swizzle(float3 c) { return SourceIsBgra ? c.bgr : c; }
float Lanczos(float x) {
    x = abs(x);
    if (x < 0.00001) return 1;
    if (x >= 3) return 0;
    float a = x * 3.14159274, b = x * 1.04719758;
    return (sin(a) / a) * (sin(b) / b);
}
int2 ClampPixel(int2 p, uint2 size) { return clamp(p, int2(0,0), int2(size)-1); }
float4 EncodedTap(float4 c);
void FilterColor(uint3 p, bool encode) {
    if (p.x >= TargetWidth || p.y >= TargetHeight) return;
    float2 a = float2(p.xy) * float2(SourceWidth,SourceHeight) / float2(TargetWidth,TargetHeight);
    float2 b = float2(p.xy+1) * float2(SourceWidth,SourceHeight) / float2(TargetWidth,TargetHeight);
    if (Peripheral) { a=UnpackPosition(p.xy); b=UnpackPosition(p.xy+1); }
    // Integrate the steady-grid footprint where the jittered frame holds it.
    a+=float2(JitterX,JitterY); b+=float2(JitterX,JitterY);
    float4 sum = 0; float weight = 0;
    for (int y=(int)floor(a.y); y<(int)ceil(b.y); ++y) {
        float wy = max(0, min(b.y,y+1.0)-max(a.y,(float)y));
        for (int x=(int)floor(a.x); x<(int)ceil(b.x); ++x) {
            float w = wy * max(0, min(b.x,x+1.0)-max(a.x,(float)x));
            float4 c=Tex0.Load(int3(ClampPixel(int2(x,y),uint2(SourceWidth,SourceHeight)),0));
            if (encode) c=EncodedTap(c);
            c.rgb=Swizzle(c.rgb); sum += c*w; weight += w;
        }
    }
    OutputColor[p.xy]=sum/max(weight,0.000001);
}
[numthreads(8,8,1)] void Downsample(uint3 p : SV_DispatchThreadID) { FilterColor(p,false); }
[numthreads(8,8,1)] void PrepareColor(uint3 p : SV_DispatchThreadID) { FilterColor(p,true); }
// Resize already prepared colour. Area filtering when reducing; bilinear
// reconstruction when increasing. Neither path performs colour conversion.
[numthreads(8,8,1)] void ResizeColor(uint3 p : SV_DispatchThreadID) {
    if (p.x>=TargetWidth || p.y>=TargetHeight) return;
    if (SourceWidth>=TargetWidth && SourceHeight>=TargetHeight) { FilterColor(p,false); return; }
    float2 q=(p.xy+0.5)*float2(SourceWidth,SourceHeight)/float2(TargetWidth,TargetHeight)-0.5;
    int2 base=(int2)floor(q); float2 f=frac(q); float4 sum=0;
    [unroll] for (int y=0;y<2;++y) [unroll] for (int x=0;x<2;++x) {
        float w=(x?f.x:1-f.x)*(y?f.y:1-f.y);
        sum+=Tex0.Load(int3(ClampPixel(base+int2(x,y),uint2(SourceWidth,SourceHeight)),0))*w;
    }
    OutputColor[p.xy]=sum;
}
)"
// MSVC limits one literal to 16 KB; adjacent literals concatenate.
R"(// Compare: original left of SplitX, NR's result from it, and a two-pixel
// black/white divider that reads on bright and dark content alike.
[numthreads(8,8,1)] void Split(uint3 p : SV_DispatchThreadID) {
    if (p.x>=TargetWidth || p.y>=TargetHeight) return;
    float4 result=Tex1.Load(int3(p.xy,0));
    if (p.x+1==SplitX || p.x==SplitX) {
        float v=((p.y/6)&1)?1:0;
        OutputColor[p.xy]=float4(v,v,v,result.a); return;
    }
    OutputColor[p.xy]=p.x<SplitX ? Tex0.Load(int3(p.xy,0)) : result;
}
[numthreads(8,8,1)] void RestoreSecond(uint3 p : SV_DispatchThreadID) {
    if (p.x>=TargetWidth || p.y>=TargetHeight) return;
    float2 q=(p.xy+0.5)*float2(SourceWidth,SourceHeight)/float2(TargetWidth,TargetHeight)-0.5;
    float3 delta=0; float weight=0;
    if (SourceWidth>TargetWidth || SourceHeight>TargetHeight) {
        // Area-average the change when pass 2 has the larger model grid.
        float2 a=p.xy*float2(SourceWidth,SourceHeight)/float2(TargetWidth,TargetHeight);
        float2 b=(p.xy+1)*float2(SourceWidth,SourceHeight)/float2(TargetWidth,TargetHeight);
        for (int y=(int)floor(a.y);y<(int)ceil(b.y);++y) for (int x=(int)floor(a.x);x<(int)ceil(b.x);++x) {
            float w=max(0,min(b.x,x+1.0)-max(a.x,(float)x))*max(0,min(b.y,y+1.0)-max(a.y,(float)y));
            int3 tap=int3(ClampPixel(int2(x,y),uint2(SourceWidth,SourceHeight)),0);
            delta+=(Tex1.Load(tap).rgb-Tex0.Load(tap).rgb)*w; weight+=w;
        }
    } else {
        // Reconstruct the change, not the image: an identity second model
        // leaves the first result exactly unchanged, including sharp detail.
        [unroll] for (int y=-2;y<=3;++y) [unroll] for (int x=-2;x<=3;++x) {
            int2 tap=(int2)floor(q)+int2(x,y); float w=Lanczos(q.x-tap.x)*Lanczos(q.y-tap.y);
            int3 at=int3(ClampPixel(tap,uint2(SourceWidth,SourceHeight)),0);
            delta+=(Tex1.Load(at).rgb-Tex0.Load(at).rgb)*w; weight+=w;
        }
    }
    float4 original=Tex2.Load(int3(p.xy,0));
    OutputColor[p.xy]=float4(original.rgb+delta/max(weight,0.000001),original.a);
}
[numthreads(8,8,1)] void PackDepth(uint3 p : SV_DispatchThreadID) {
    if (p.x>=WorkWidth || p.y>=WorkHeight) return;
    float2 native=UnpackPosition(p.xy+0.5);
    int2 guide=ClampPixel(int2(native*float2(GuideWidth,GuideHeight)/float2(SourceWidth,SourceHeight)),uint2(GuideWidth,GuideHeight));
    OutputDepth[p.xy]=Tex0.Load(int3(guide,0)).r;
}
[numthreads(8,8,1)] void PackMotion(uint3 p : SV_DispatchThreadID) {
    if (p.x>=WorkWidth || p.y>=WorkHeight) return;
    float2 native=UnpackPosition(p.xy+0.5);
    int2 guide=ClampPixel(int2(native*float2(GuideWidth,GuideHeight)/float2(SourceWidth,SourceHeight)),uint2(GuideWidth,GuideHeight));
    // Host scales describe guide pixels. Convert to scene pixels before
    // applying the nonlinear transform to both current/previous endpoints.
    float2 motion=Tex0.Load(int3(guide,0)).rg*float2(MotionScaleX,MotionScaleY)*
        float2(SourceWidth,SourceHeight)/float2(GuideWidth,GuideHeight);
    OutputMotion[p.xy]=PackPosition(native+motion)-PackPosition(native);
}
[numthreads(8,8,1)] void PackGuides(uint3 p : SV_DispatchThreadID) {
    if (p.x>=WorkWidth || p.y>=WorkHeight) return;
    float2 native=UnpackPosition(p.xy+0.5);
    int2 guide=ClampPixel(int2(native*float2(GuideWidth,GuideHeight)/float2(SourceWidth,SourceHeight)),uint2(GuideWidth,GuideHeight));
    OutputDepth[p.xy]=Tex0.Load(int3(guide,0)).r;
    float2 motion=Tex1.Load(int3(guide,0)).rg*float2(MotionScaleX,MotionScaleY)*
        float2(SourceWidth,SourceHeight)/float2(GuideWidth,GuideHeight);
    OutputSecondMotion[p.xy]=PackPosition(native+motion)-PackPosition(native);
}
[numthreads(8,8,1)] void Residual(uint3 p : SV_DispatchThreadID) {
    if (Mode == 0) {
        // Horizontal difference, low width -> native width, height stays low.
        if (p.x >= SourceWidth || p.y >= TargetHeight) return;
        float3 sum=0; float weight=0;
        if (SourceWidth == TargetWidth && !Peripheral) {
            sum = Tex1.Load(int3(p.xy,0)).rgb - Tex0.Load(int3(p.xy,0)).rgb; weight=1;
        } else {
            float x=(p.x+0.5)*TargetWidth/SourceWidth-0.5;
            if (Peripheral) x=PackAxis(p.x+0.5,SourceWidth,TargetWidth)-0.5;
            [unroll] for (int i=-2; i<=3; ++i) {
                int sx=(int)floor(x)+i; float w=Lanczos(x-sx);
                int3 q=int3(clamp(sx,0,(int)TargetWidth-1),p.y,0);
                sum+=(Tex1.Load(q).rgb-Tex0.Load(q).rgb)*w; weight+=w;
            }
        }
        OutputColor[p.xy]=float4(sum/(abs(weight)>0.000001?weight:1),0); return;
    }
    if (p.x >= SourceWidth || p.y >= SourceHeight) return;
    float4 original=Tex0.Load(int3(p.xy,0));
    float3 delta=0; float weight=0;
    if (SourceHeight == TargetHeight && !Peripheral) { delta=Tex1.Load(int3(p.xy,0)).rgb; weight=1; }
    else {
        float y=(p.y+0.5)*TargetHeight/SourceHeight-0.5;
        if (Peripheral) y=PackAxis(p.y+0.5,SourceHeight,TargetHeight)-0.5;
        [unroll] for (int i=-2; i<=3; ++i) {
            int sy=(int)floor(y)+i; float w=Lanczos(y-sy);
            delta+=Tex1.Load(int3(p.x,clamp(sy,0,(int)TargetHeight-1),0)).rgb*w; weight+=w;
        }
    }
    OutputColor[p.xy]=float4(Swizzle(saturate(Swizzle(original.rgb)+delta/(abs(weight)>0.000001?weight:1))),original.a);
}
float Luma(float3 c) { return dot(c,float3(0.212599993,0.715200007,0.0722000003)); }
// The white point linear HDR input is divided by: fixed, or measured per frame.
float White() { return max(AdaptiveExposure ? ExposureTex.Load(int3(0,0,0)) : WhitePoint, 0.0001); }
float3 ToLinear(float3 c) {
    c=saturate(c);
    return lerp(c*0.0773993805, pow((c+0.0549999997)*0.947867334,2.40000010),step(0.0404499993,c));
}
float3 ToSRGB(float3 c) {
    c=saturate(c);
    return lerp(c*12.92,1.055*pow(max(c,0),0.416666657)-0.055,step(0.0031308,c));
}
float3 ToLab(float3 c) {
    float3 lms=float3(dot(c,float3(0.412221462,0.536332548,0.0514459945)),
        dot(c,float3(0.211903498,0.680699527,0.107396960)),dot(c,float3(0.0883024633,0.281718850,0.629978716)));
    lms=sign(lms)*pow(abs(lms),0.333333343);
    return float3(dot(lms,float3(0.210454255,0.793617785,-0.00407204684)),
        dot(lms,float3(1.97799850,-2.42859221,0.450593710)),dot(lms,float3(0.0259040371,0.782771766,-0.808675766)));
}
float3 FromLab(float3 c) {
    float3 lms=float3(dot(c,float3(1,0.396337777,0.215803757)),
        dot(c,float3(1,-0.105561346,-0.0638541728)),dot(c,float3(1,-0.0894841775,-1.29148555)));
    lms=lms*lms*lms;
    return float3(dot(lms,float3(4.07674170,-3.30771160,0.230969936)),
        dot(lms,float3(-1.26843798,2.60975742,-0.341319382)),dot(lms,float3(-0.00419608643,-0.703418612,1.70761466)));
}
float3 GamutClip(float3 c) {
    float3 wide=float3(dot(c,float3(0.613097012,0.339522988,0.0473789982)),
        dot(c,float3(0.0701939985,0.916354001,0.0134520000)),dot(c,float3(0.0206160005,0.109569997,0.869814992)));
    wide=max(wide,0);
    return float3(dot(wide,float3(1.70505095,-0.621792018,-0.0832590014)),
        dot(wide,float3(-0.130255997,1.14080501,-0.0105480002)),dot(wide,float3(-0.0240030009,-0.128968999,1.15297198)));
}
// NR's model carries a small, consistent colour bias (a green cast on grey
// shading). Where the original is near neutral, NR may change its saturation
// along the original's own hue, never rotate it or push it past neutral; the
// guard releases as the original carries chroma of its own (OkLab 0.02-0.08).
// Lightness is kept. Linear RGB in and out.
float3 HueGuard(float3 original, float3 result) {
    float3 o=ToLab(max(original,0)), r=ToLab(max(result,0));
    float chroma=length(o.yz), keep=1-smoothstep(0.02,0.08,chroma);
    if (keep<=0) return result;
    float2 axis=chroma>0.000001 ? o.yz/chroma : float2(0,0);
    r.yz=lerp(r.yz,axis*max(dot(r.yz,axis),0),keep);
    return GamutClip(FromLab(r));
}
// Per frame, not per pixel: black is a legitimate answer (a shadow NR paints
// over a lit surface). Only a model output with nothing anywhere on an 8x8
// grid where NR's input had content, such as an empty first evaluation, counts
// as no answer. A near-black scene gives near-black input, so its dark answer
// is kept rather than switching between NR and the scene from frame to frame.
// Every thread of the group must call this; Mode is uniform.
groupshared uint AnswerSeen;
bool ModelAnswered(uint index) {
    if (index==0) AnswerSeen=0;
    GroupMemoryBarrierWithGroupSync();
    uint2 size=max(uint2(WorkWidth,WorkHeight),1);
    uint2 q=min(uint2((float2(index%8,index/8)+0.5)*float2(size)/8.0),size-1);
    float3 n=Tex1.Load(int3(q,0)).rgb, a=Tex0.Load(int3(q,0)).rgb;
    uint seen=0;
    if (all(isfinite(n)) && max(n.r,max(n.g,n.b))>0.00001) seen|=1u;
    if (all(isfinite(a)) && max(a.r,max(a.g,a.b))>4.0/255.0) seen|=2u;
    if (seen) InterlockedOr(AnswerSeen,seen);
    GroupMemoryBarrierWithGroupSync();
    return (AnswerSeen&1u)!=0 || (AnswerSeen&2u)==0;
}
// A paired range conversion in the producer's own RGB coordinates. No gamma,
// gamut or exposure interpretation is inferred from its floating-point format.
// Reuse the original pixel's scale when returning NR's delta; never invert a
// nearly-white learned value, which would amplify quantization unpredictably.
float ProducerScale(float3 c) {
    float peak=max(c.r,max(c.g,c.b));
    float white=max(WhitePoint,0.0001), x=peak/white;
    if (x<=0.75) return white;
    float bounded=0.75+0.25*(1-exp2(-5.77078009*(x-0.75)));
    return peak/bounded;
}
float4 EncodedTap(float4 c) {
    float3 rgb=max(Swizzle(c.rgb),0);
    if (ProducerColor) {
        rgb=all(isfinite(c.rgb)) ? rgb/ProducerScale(rgb) : 0;
        c=float4(Swizzle(saturate(rgb)),1);
    } else {
        if (!Passthrough) {
            rgb/=White();
            float peak=max(rgb.r,max(rgb.g,rgb.b));
            if (peak>0.75) rgb*=(0.75+0.25*(1-exp2(-5.77078009*(peak-0.75))))/peak;
            rgb=ToSRGB(rgb);
        }
        c=float4(Swizzle(rgb),c.a);
    }
    // Match the eliminated intermediate texture's storage before filtering.
    if (EncodedFormat==1) c=f16tof32(f32tof16(c));
    else if (EncodedFormat==2) c=round(saturate(c)*255)/255;
    return c;
}
float3 RestoreProducer(float3 original, float3 a, float3 n) {
    if (!all(isfinite(original)) || !all(isfinite(a)) || !all(isfinite(n)) || TransferStrength==0) return original;
    float3 base=max(original,0);
    float peak=max(base.r,max(base.g,base.b));
    if (peak<=0.000001) return original;
    a=saturate(a); n=saturate(n);
    float3 delta=(n-a)*ProducerScale(base);
    float brightnessRatio=(Luma(n)+0.000001)/(Luma(a)+0.000001);
    float3 brightnessDelta=base*(brightnessRatio-1);
    float3 result=base+TransferStrength*lerp(brightnessDelta,delta,ColourStrength);
    if (!all(isfinite(result))) return original;
    // CS completes the scene in 2.2-gamma RGB; judge hue in linear light. An
    // unchanged pixel skips the round trip so a no-op model stays exact.
    if (any(result!=base)) result=pow(max(HueGuard(pow(base,2.2),pow(max(result,0),2.2)),0),1/2.2);
    // Bound gain and prevent FP16 overflow. The original signed channels and
    // alpha are retained outside the nonnegative model proxy.
    float limit=min(peak*max(MaxRatio,1),max(peak,65504));
    result=clamp(result,0,limit);
    return float3(original.r<0?original.r:result.r,
                  original.g<0?original.g:result.g,
                  original.b<0?original.b:result.b);
}
)"
R"(// Catmull-Rom at a source position (pixel centres at +0.5), clamped to the
// 2x2 neighbourhood so HDR highlights cannot ring. Moves the jittered frame
// onto the steady grid NR sees; the resolve moves NR's change back.
float4 SampleSteady(float2 position) {
    float2 q=position-0.5, f=frac(q);
    int2 base=int2(floor(q));
    float2 w[4]={ f*(-0.5+f*(1-0.5*f)), 1+f*f*(-2.5+1.5*f), f*(0.5+f*(2-1.5*f)), f*f*(-0.5+0.5*f) };
    float4 sum=0, low=65504, high=-65504;
    [unroll] for (int y=0; y<4; ++y) {
        [unroll] for (int x=0; x<4; ++x) {
            float4 tap=Tex0.Load(int3(ClampPixel(base+int2(x-1,y-1),uint2(SourceWidth,SourceHeight)),0));
            tap=all(isfinite(tap)) ? tap : 0;
            sum+=tap*w[x].x*w[y].y;
            if ((x==1||x==2) && (y==1||y==2)) { low=min(low,tap); high=max(high,tap); }
        }
    }
    return clamp(sum,low,high);
}
[numthreads(8,8,1)] void Ratio(uint3 p : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    const bool answered = Mode == 1 ? ModelAnswered(index) : true;
    if (p.x >= TargetWidth || p.y >= TargetHeight) return;
    const float2 jitter=float2(JitterX,JitterY);
    const bool jittered=any(jitter!=0);
    if (Mode == 0) {
        float4 c=jittered ? SampleSteady(p.xy+0.5+jitter) : Tex0.Load(int3(p.xy,0));
        float3 rgb=max(Swizzle(c.rgb),0);
        if (ProducerColor) {
            rgb=all(isfinite(c.rgb)) ? rgb/ProducerScale(rgb) : 0;
            OutputColor[p.xy]=float4(Swizzle(saturate(rgb)),1); return;
        }
        if (!Passthrough) {
            rgb/=White();
            // Bound the brightest channel with one RGB scale. A luminance-only
            // shoulder lets saturated highlights exceed one; ToSRGB then clips
            // channels separately and changes hue even when NR changes nothing.
            float peak=max(rgb.r,max(rgb.g,rgb.b));
            if (peak>0.75) rgb*= (0.75+0.25*(1-exp2(-5.77078009*(peak-0.75))))/peak;
            rgb=ToSRGB(rgb);
        }
        OutputColor[p.xy]=float4(Swizzle(rgb),c.a); return;
    }
    float4 original=Tex2.Load(int3(p.xy,0));
    // No answer at all (an empty first evaluation): keep the scene. Producer
    // restoration and the tone bands would otherwise read it as an edit.
    if (!answered) { OutputColor[p.xy]=original; return; }
    float3 a=0, n=0;
    uint2 size=max(uint2(WorkWidth,WorkHeight),1);
    // Where this pixel samples NR's grid; the tone bands are read there too.
    float2 tonePos=float2(p.xy);
    if (all(size==uint2(TargetWidth,TargetHeight)) && !Peripheral && !jittered) {
        a=Swizzle(Tex0.Load(int3(p.xy,0)).rgb); n=Swizzle(Tex1.Load(int3(p.xy,0)).rgb);
    } else {
        // This jittered pixel sits at p - jitter on the steady grid NR saw.
        float2 steady=p.xy+0.5-jitter;
        float2 pos=steady*size/float2(TargetWidth,TargetHeight)-0.5; float weight=0;
        if (Peripheral) pos=PackPosition(steady)-0.5;
        tonePos=pos;
        [loop] for (int y=-2; y<=3; ++y) {
            [loop] for (int x=-2; x<=3; ++x) {
                int2 q=int2(floor(pos))+int2(x,y);
                float w=Lanczos(pos.x-q.x)*Lanczos(pos.y-q.y);
                q=ClampPixel(q,size);
                a+=Swizzle(Tex0.Load(int3(q,0)).rgb)*w; n+=Swizzle(Tex1.Load(int3(q,0)).rgb)*w; weight+=w;
            }
        }
        weight=abs(weight)>0.000001?weight:1; a/=weight; n/=weight;
    }
    if (ProducerColor) {
        OutputColor[p.xy]=float4(Swizzle(RestoreProducer(Swizzle(original.rgb),a,n)),original.a); return;
    }
    if (TransferStrength==0) { OutputColor[p.xy]=original; return; }
    if (!Passthrough) { a=ToLinear(a); n=ToLinear(n); }
    float white=Passthrough?1:White();
    float3 base=Swizzle(original.rgb)/white, result=base;
    float by=Luma(base), ny=Luma(n), ay=Luma(a);
    if (ny>0.00001) {
        float ratio=by<ay?by/max(ay,0.000001):(max(by-ay,0)+ny)/ny;
        float3 scaled=ToLab(n*ratio), normal=ToLab(n);
        float chroma=length(normal.yz);
        scaled.yz=normal.yz*(chroma==0?1:length(scaled.yz)/chroma);
        result=lerp(base,GamutClip(FromLab(scaled)),TransferStrength);
    } else if (answered) {
        // NR painted this pixel black: darken the original by the same ratio.
        result=lerp(base,base*(max(ny,0)+0.0001)/(max(ay,0)+0.0001),TransferStrength);
    }
    float lumaRatio=max((Luma(result)+0.001953125)/(max(by,0)+0.001953125),0);
    result=max(lerp(base*lumaRatio,result,ColourStrength),0);
    if (ToneBands) {
        // NR's change in stops splits into the blurred (broad) part and the rest
        // (detail); scale each. Broad and Detail at 1 leave the result unchanged.
        float delta=log2((max(ny,0)+0.001953125)/(max(ay,0)+0.001953125));
        float broad=ToneTex.Load(int3(clamp(int2(floor(tonePos+0.5)),int2(0,0),int2(size)-1),0)).y;
        float tone=(Broad-1)*broad+(Detail-1)*(delta-broad);
        result*=exp2(clamp(tone,-4,4));
    }
    result=HueGuard(base,result);
    // Limit the final colour, including extrapolation above colour strength 1.
    // Capping only the luminance branch lets the other branch bypass the limit.
    float resultY=Luma(result), maxY=max(by,0)*MaxRatio;
    if (resultY>maxY) result*=maxY/max(resultY,0.0000000001);
    result=min(result*white,65504);
    OutputColor[p.xy]=float4(Swizzle(result),original.a);
}
// Adaptive exposure. The geometric mean luminance of a 64x64 grid over the
// source maps to mid-grey, shifted by the bias in EV, and adapts in log space
// at ExposureRate per frame (snapping on reset). Writes the white point to the
// 1x1 output, which the encode and Ratio passes then read. One 16x16 group.
groupshared float ExposureSum[256];
[numthreads(16,16,1)] void MeasureExposure(uint3 t : SV_GroupThreadID, uint index : SV_GroupIndex) {
    float sum=0;
    [unroll] for (uint y=0; y<4; ++y) {
        [unroll] for (uint x=0; x<4; ++x) {
            float2 cell=float2(t.xy*4+uint2(x,y))+0.5;
            uint2 q=min(uint2(cell*float2(SourceWidth,SourceHeight)/64.0),uint2(SourceWidth,SourceHeight)-1);
            float3 c=Swizzle(Tex0.Load(int3(q,0)).rgb);
            sum+=log2(max(all(isfinite(c)) ? Luma(max(c,0)) : 0, 0.0001));
        }
    }
    ExposureSum[index]=sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s=128; s>0; s>>=1) {
        if (index<s) ExposureSum[index]+=ExposureSum[index+s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index==0) {
        float target=ExposureSum[0]/4096.0-log2(0.18)-ExposureBias;
        float previous=OutputDepth[uint2(0,0)];
        bool usable=previous>0 && isfinite(previous) && !ExposureReset;
        float current=usable ? lerp(log2(previous),target,saturate(ExposureRate)) : target;
        OutputDepth[uint2(0,0)]=exp2(clamp(current,-20,20));
    }
}
// Broad and Detail, at NR's grid: (log2 proxy luminance, NR's change in stops).
// Tex0 is the proxy NR saw, Tex1 NR's answer, both display-encoded unless
// Passthrough; luminance is judged in linear light with a shared shadow floor.
[numthreads(8,8,1)] void PrepareTone(uint3 p : SV_DispatchThreadID) {
    if (p.x>=TargetWidth || p.y>=TargetHeight) return;
    float3 a=Swizzle(Tex0.Load(int3(p.xy,0)).rgb), n=Swizzle(Tex1.Load(int3(p.xy,0)).rgb);
    if (!Passthrough) { a=ToLinear(a); n=ToLinear(n); }
    float ay=max(all(isfinite(a)) ? Luma(a) : 0,0)+0.001953125, ny=max(all(isfinite(n)) ? Luma(n) : 0,0)+0.001953125;
    OutputMotion[p.xy]=float2(log2(ay),log2(ny/ay));
}
// Separable edge-aware blur of NR's change (Mode 0 horizontal, 1 vertical):
// 17 taps spaced so the radius is about 1/12 of the grid's width, Gaussian in
// distance and falling off across edges in the proxy's own brightness.
[numthreads(8,8,1)] void FilterTone(uint3 p : SV_DispatchThreadID) {
    if (p.x>=TargetWidth || p.y>=TargetHeight) return;
    int step=max(1,int(TargetWidth/96));
    int2 axis=Mode==0 ? int2(step,0) : int2(0,step);
    float2 centre=Tex0.Load(int3(p.xy,0)).xy;
    float sum=0, weight=0;
    [unroll] for (int i=-8; i<=8; ++i) {
        float2 tap=Tex0.Load(int3(ClampPixel(int2(p.xy)+axis*i,uint2(TargetWidth,TargetHeight)),0)).xy;
        float w=exp(-i*i/32.0)*exp(-3*abs(tap.x-centre.x));
        sum+=tap.y*w; weight+=w;
    }
    OutputMotion[p.xy]=float2(centre.x,sum/max(weight,0.000001));
}
)"
R"(// Model rate: every other frame. After cavy8's Alternate Frames for Community
// Shaders NR (Bottled Shaders PR #6): reuse the previous evaluation along the
// game's motion, reject it by depth and object group, and give the model two
// frames of motion after a skipped one.
//
// Guides, one RGBA16F texel per guide texel: motion in guide pixels (an offset,
// so FP16 keeps it within 1/16 pixel below 128 pixels), log2 distance from the
// far plane (about reciprocal view depth; the log keeps FP16's relative
// precision for distant terrain) and the object group (0 world, 1-4 objects).
Texture2D<float4> PreviousGuides : register(t3);
static const int2 kRing[8]={int2(-1,-1),int2(0,-1),int2(1,-1),int2(-1,0),int2(1,0),int2(-1,1),int2(0,1),int2(1,1)};
float FarDistanceLog(float depth) {
    float s=DepthInverted ? depth : 1-depth;
    return log2(clamp(isfinite(s) ? s : 0,0.0000000596,1));
}
// The object classification's packing and depth test (NeuralObjectMask.h).
float ObjectGroupOf(float objects, float depth) {
    uint packed=asuint(objects), group=packed&7u; float objectDepth=asfloat(packed&~7u);
    return isfinite(objectDepth)&&isfinite(depth)&&group>=1&&group<=4&&
        abs(objectDepth-depth)<=max(0.00000024,abs(depth)*0.000002) ? group : 0;
}
// 1 where a stored guide shows the same surface as this frame's: reciprocal
// depth within about 10%, fully rejected beyond about 30%, so foreground and
// background swaps fail while ordinary camera moves pass; and the same object
// group when both frames classified objects.
float SameSurface(float4 current, float4 previous) {
    if (!all(isfinite(previous)) || (Categories && current.w!=previous.w)) return 0;
    return 1-smoothstep(0.15,0.4,abs(current.z-previous.z));
}
// The guide texel under a colour-raster position (pixel centres at +0.5).
int2 GuideTexel(float2 position, float2 jitter) {
    float2 g=position*float2(GuideWidth,GuideHeight)/float2(SourceWidth,SourceHeight);
    if (GuidesJittered) g+=jitter;
    return ClampPixel(int2(floor(g)),uint2(GuideWidth,GuideHeight));
}
// Tex0 motion, Tex1 depth, Tex2 the object classification or nothing.
[numthreads(8,8,1)] void CaptureGuides(uint3 p : SV_DispatchThreadID) {
    if (p.x>=GuideWidth || p.y>=GuideHeight) return;
    float2 motion=Tex0.Load(int3(p.xy,0)).rg*float2(MotionScaleX,MotionScaleY);
    float limit=4.0*max(GuideWidth,GuideHeight);
    motion=all(isfinite(motion)) ? clamp(motion,-limit,limit) : limit;
    float depth=Tex1.Load(int3(p.xy,0)).r;
    OutputColor[p.xy]=float4(motion,FarDistanceLog(depth),ObjectGroupOf(Tex2.Load(int3(p.xy,0)).r,depth));
}
// The model's motion on an evaluation after a skipped frame, whose history is
// two frames old: mv2(p) = mv(p) + mv'(p + mv(p)), where mv' is the skipped
// frame's motion found where this surface was on it, across the jitter
// difference of the two guide rasters. A surface the skipped frame did not
// show has no history; it points off screen. Output in the raw motion units.
// Tex0 this frame's raw motion, Tex1 its guides, Tex2 the skipped frame's.
[numthreads(8,8,1)] void ComposeMotion(uint3 p : SV_DispatchThreadID) {
    if (p.x>=GuideWidth || p.y>=GuideHeight) return;
    float2 scale=float2(MotionScaleX,MotionScaleY);
    float2 motion=Tex0.Load(int3(p.xy,0)).rg*scale;
    float2 composed=2*float2(GuideWidth,GuideHeight);
    if (all(isfinite(motion))) {
        float2 position=p.xy+0.5+motion+float2(PreviousJitterX-CameraJitterX,PreviousJitterY-CameraJitterY);
        int2 q=int2(floor(position));
        if (all(position>=0) && all(q<int2(GuideWidth,GuideHeight))) {
            float4 skipped=Tex2.Load(int3(q,0));
            if (SameSurface(Tex1.Load(int3(p.xy,0)),skipped)>0.5) composed=motion+skipped.xy;
        }
    }
    OutputMotion[p.xy]=composed/scale;
}
// Brightness code shared by StoreChange and Reproject: display-encoded luma,
// or linear HDR luma relative to the white point with a 2.2 gamma.
float ChangeCode(float3 c) {
    float y=Luma(Swizzle(max(c,0)));
    if (ProducerColor) return y/max(WhitePoint,0.0001);
    return Passthrough ? y : pow(y/max(WhitePoint,0.0001),0.454545468);
}
// NR's change at source resolution relative to its input, per channel:
// (result + floor) / (input + floor), clamped to 1/64..64. A ratio carries the
// change onto a frame whose brightness differs a little, display-encoded UNORM
// and linear FP16 alike; the floor makes it additive near black, where a plain
// ratio would amplify noise. FP16 stores it within about 0.05%. Alpha keeps
// the input's brightness code for Reproject. Tex0 the input, Tex1 NR's result.
[numthreads(8,8,1)] void StoreChange(uint3 p : SV_DispatchThreadID) {
    if (p.x>=SourceWidth || p.y>=SourceHeight) return;
    float4 input=Tex0.Load(int3(p.xy,0)), result=Tex1.Load(int3(p.xy,0));
    float3 ratio=(result.rgb+ChangeFloor)/(input.rgb+ChangeFloor);
    ratio=input.rgb>=0 && isfinite(ratio) ? clamp(ratio,0.015625,64) : 1;
    OutputColor[p.xy]=float4(ratio,all(isfinite(input.rgb)) ? ChangeCode(input.rgb) : 0);
}
// A frame without the model: this frame's input with the previous frame's
// change, read where this surface was on the previous frame (its motion, and
// the jitter difference before upscaling). Taps showing another surface are
// rejected by depth and object group. When none of the four bilinear taps
// remains (newly revealed or off screen), the best matching tap within three
// guide texels is used, else the input is shown unchanged. The change fades
// where the input's brightness no longer matches (lights, animated surfaces).
// Tex0 this frame's input, Tex1 the stored change, Tex2 this frame's guides,
// PreviousGuides the previous frame's.
[numthreads(8,8,1)] void Reproject(uint3 p : SV_DispatchThreadID) {
    if (p.x>=SourceWidth || p.y>=SourceHeight) return;
    float4 input=Tex0.Load(int3(p.xy,0));
    const float2 jitter=float2(CameraJitterX,CameraJitterY), previousJitter=float2(PreviousJitterX,PreviousJitterY);
    float4 current=Tex2.Load(int3(GuideTexel(p.xy+0.5,jitter),0));
    float2 scale=float2(SourceWidth,SourceHeight)/float2(GuideWidth,GuideHeight);
    // Before upscaling the colour raster itself moves with the jitter.
    float2 position=p.xy+0.5+current.xy*scale+(GuidesJittered ? float2(0,0) : previousJitter-jitter);
    float4 change=float4(1,1,1,0); bool found=false;
    if (all(isfinite(position))) {
        float2 b=position-0.5; int2 base=int2(floor(b)); float2 f=b-base;
        float4 sum=0; float weight=0;
        [unroll] for (int y=0;y<2;++y) [unroll] for (int x=0;x<2;++x) {
            int2 t=base+int2(x,y);
            float w=(x?f.x:1-f.x)*(y?f.y:1-f.y);
            if (w<=0 || any(t<0) || any(t>=int2(SourceWidth,SourceHeight))) continue;
            w*=SameSurface(current,PreviousGuides.Load(int3(GuideTexel(t+0.5,previousJitter),0)));
            sum+=Tex1.Load(int3(t,0))*w; weight+=w;
        }
        if (weight>0.05) { change=sum/weight; found=true; }
        else {
            float best=0.5;
            [loop] for (int ring=1; ring<=3 && !found; ++ring) {
                [unroll] for (int i=0;i<8;++i) {
                    int2 t=ClampPixel(int2(floor(position+kRing[i]*ring*scale)),uint2(SourceWidth,SourceHeight));
                    float s=SameSurface(current,PreviousGuides.Load(int3(GuideTexel(t+0.5,previousJitter),0)));
                    if (s>best) { best=s; change=Tex1.Load(int3(t,0)); found=true; }
                }
            }
        }
    }
    float code=all(isfinite(input.rgb)) ? ChangeCode(input.rgb) : 0;
    float match=1-smoothstep(0.1,0.3,abs(code-change.a)/(code+change.a+0.05));
    float3 ratio=found ? lerp(1,change.rgb,match) : 1;
    float3 result=(input.rgb+ChangeFloor)*ratio-ChangeFloor;
    result=input.rgb>=0 && ratio!=1 && isfinite(result) ? min(max(result,0),65504) : input.rgb;
    OutputColor[p.xy]=float4(result,input.a);
}
)";
}
