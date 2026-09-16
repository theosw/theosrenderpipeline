#pragma once
namespace TheosRenderPipeline::SourceDLSSG
{
// Area downsampling, separable Lanczos3 residual and luma/OkLab ratio transfer.
// Ratio transfer, hue correction and AP1 clipping correspond to RenoDX's public
// UpgradeToneMap, HueOKLab and clamp::AP1 components (MIT).
// Copyright (c) 2025 Carlos Lopez Jr. See THIRD-PARTY.md for the full notice,
// pinned upstream links and component scope; OkLab is defined by Bjorn Ottosson.
inline constexpr char kNeuralResolveShader[] = R"(
cbuffer NRParams : register(b0) {
    uint SourceWidth, SourceHeight, TargetWidth, TargetHeight;
    uint SourceIsBgra, Mode, Passthrough, Pad0;
    uint WorkWidth, WorkHeight;
    float TransferStrength, ColourStrength, MaxRatio, WhitePoint;
};
Texture2D<float4> Tex0 : register(t0);
Texture2D<float4> Tex1 : register(t1);
Texture2D<float4> Tex2 : register(t2);
RWTexture2D<float4> OutputColor : register(u0);
float3 Swizzle(float3 c) { return SourceIsBgra ? c.bgr : c; }
float Lanczos(float x) {
    x = abs(x);
    if (x < 0.00001) return 1;
    if (x >= 3) return 0;
    float a = x * 3.14159274, b = x * 1.04719758;
    return (sin(a) / a) * (sin(b) / b);
}
int2 ClampPixel(int2 p, uint2 size) { return clamp(p, int2(0,0), int2(size)-1); }
[numthreads(8,8,1)] void Downsample(uint3 p : SV_DispatchThreadID) {
    if (p.x >= TargetWidth || p.y >= TargetHeight) return;
    float2 a = float2(p.xy) * float2(SourceWidth,SourceHeight) / float2(TargetWidth,TargetHeight);
    float2 b = float2(p.xy+1) * float2(SourceWidth,SourceHeight) / float2(TargetWidth,TargetHeight);
    float4 sum = 0; float weight = 0;
    for (int y=(int)floor(a.y); y<(int)ceil(b.y); ++y) {
        float wy = max(0, min(b.y,y+1.0)-max(a.y,(float)y));
        for (int x=(int)floor(a.x); x<(int)ceil(b.x); ++x) {
            float w = wy * max(0, min(b.x,x+1.0)-max(a.x,(float)x));
            float4 c=Tex0.Load(int3(ClampPixel(int2(x,y),uint2(SourceWidth,SourceHeight)),0));
            c.rgb=Swizzle(c.rgb); sum += c*w; weight += w;
        }
    }
    OutputColor[p.xy]=sum/max(weight,0.000001);
}
[numthreads(8,8,1)] void Residual(uint3 p : SV_DispatchThreadID) {
    if (Mode == 0) {
        // Horizontal difference, low width -> native width, height stays low.
        if (p.x >= SourceWidth || p.y >= TargetHeight) return;
        float3 sum=0; float weight=0;
        if (SourceWidth == TargetWidth) {
            sum = Tex1.Load(int3(p.xy,0)).rgb - Tex0.Load(int3(p.xy,0)).rgb; weight=1;
        } else {
            float x=(p.x+0.5)*TargetWidth/SourceWidth-0.5;
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
    if (SourceHeight == TargetHeight) { delta=Tex1.Load(int3(p.xy,0)).rgb; weight=1; }
    else {
        float y=(p.y+0.5)*TargetHeight/SourceHeight-0.5;
        [unroll] for (int i=-2; i<=3; ++i) {
            int sy=(int)floor(y)+i; float w=Lanczos(y-sy);
            delta+=Tex1.Load(int3(p.x,clamp(sy,0,(int)TargetHeight-1),0)).rgb*w; weight+=w;
        }
    }
    OutputColor[p.xy]=float4(Swizzle(saturate(Swizzle(original.rgb)+delta/(abs(weight)>0.000001?weight:1))),original.a);
}
float Luma(float3 c) { return dot(c,float3(0.212599993,0.715200007,0.0722000003)); }
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
[numthreads(8,8,1)] void Ratio(uint3 p : SV_DispatchThreadID) {
    if (p.x >= TargetWidth || p.y >= TargetHeight) return;
    if (Mode == 0) {
        float4 c=Tex0.Load(int3(p.xy,0)); float3 rgb=max(Swizzle(c.rgb),0);
        if (!Passthrough) {
            rgb/=max(WhitePoint,0.0001); float y=Luma(rgb);
            if (y>0.75) rgb*= (0.75+0.25*(1-exp2(-5.77078009*(y-0.75))))/y;
            rgb=ToSRGB(rgb);
        }
        OutputColor[p.xy]=float4(Swizzle(rgb),c.a); return;
    }
    float4 original=Tex2.Load(int3(p.xy,0));
    float3 a=0, n=0;
    uint2 size=max(uint2(WorkWidth,WorkHeight),1);
    if (all(size==uint2(TargetWidth,TargetHeight))) {
        a=Swizzle(Tex0.Load(int3(p.xy,0)).rgb); n=Swizzle(Tex1.Load(int3(p.xy,0)).rgb);
    } else {
        float2 pos=(p.xy+0.5)*size/float2(TargetWidth,TargetHeight)-0.5; float weight=0;
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
    if (!Passthrough) { a=ToLinear(a); n=ToLinear(n); }
    float white=Passthrough?1:max(WhitePoint,0.0001);
    float3 base=Swizzle(original.rgb)/white, result=base;
    float by=Luma(base), ny=Luma(n), ay=Luma(a);
    if (ny>0.00001) {
        float ratio=by<ay?by/max(ay,0.000001):(max(by-ay,0)+ny)/ny;
        float3 scaled=ToLab(n*ratio), normal=ToLab(n);
        float chroma=length(normal.yz);
        scaled.yz=normal.yz*(chroma==0?1:length(scaled.yz)/chroma);
        result=lerp(base,GamutClip(FromLab(scaled)),TransferStrength);
    }
    float lumaRatio=min(max((Luma(result)+0.001953125)/(by+0.001953125),0),MaxRatio);
    result=max(lerp(base*lumaRatio,result,ColourStrength)*white,0);
    OutputColor[p.xy]=float4(Swizzle(result),original.a);
}
)";
}
