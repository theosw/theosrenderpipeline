#pragma once
// Independent implementation of delayed residual composition. This prototype
// uses current-colour/depth rejection, not upstream's borrowed-displacement
// filling. Rejected pixels deliberately retain the original scene.
inline constexpr char kAsyncShader[] = R"(
Texture2D<float4> A:register(t0);
Texture2D<float4> B:register(t1);
Texture2D<float4> C:register(t2);
Texture2D<float4> D:register(t3);
Texture2D<float4> E:register(t4);
Texture2D<float4> F:register(t5);
RWTexture2D<float4> Out:register(u0);
cbuffer Constants:register(b0) {
    uint Width, Height, Initialize, UseResult;
    float ScaleX, ScaleY, DepthTolerance, ColorTolerance;
    float MaxRatio; uint Age, MaxAge, Padding;
    uint GuideWidth, GuideHeight, Padding2, Padding3;
};
bool Inside(float2 p) {
    return all(isfinite(p)) && all(p >= .5) && all(p <= float2(Width,Height)-.5);
}
float4 Sample(Texture2D<float4> tex,float2 p) {
    float2 q=p-.5; int2 lo=int2(floor(q)); float2 t=frac(q);
    int2 hi=int2(Width,Height)-1;
    return lerp(lerp(tex.Load(int3(clamp(lo,0,hi),0)),tex.Load(int3(clamp(lo+int2(1,0),0,hi),0)),t.x),
                lerp(tex.Load(int3(clamp(lo+int2(0,1),0,hi),0)),tex.Load(int3(clamp(lo+1,0,hi),0)),t.x),t.y);
}
float GuideDepth(Texture2D<float4> tex,float2 p) {
    float2 q=p*float2(GuideWidth,GuideHeight)/float2(Width,Height)-.5;
    int2 lo=int2(floor(q)), hi=int2(GuideWidth,GuideHeight)-1; float2 t=frac(q);
    return lerp(lerp(tex.Load(int3(clamp(lo,0,hi),0)).r,tex.Load(int3(clamp(lo+int2(1,0),0,hi),0)).r,t.x),
                lerp(tex.Load(int3(clamp(lo+int2(0,1),0,hi),0)).r,tex.Load(int3(clamp(lo+1,0,hi),0)).r,t.x),t.y);
}
 bool DepthMatches(float a,float b) {
    return isfinite(a) && isfinite(b) && abs(a-b)<=DepthTolerance*max(max(abs(a),abs(b)),1e-6);
}
[numthreads(8,8,1)] void Track(uint3 id:SV_DispatchThreadID) {
    if(id.x>=Width || id.y>=Height)return;
    if(Initialize){Out[id.xy]=float4(0,0,1,0);return;}
    float2 mv=A.Load(int3(min(uint2((id.xy+.5)*float2(GuideWidth,GuideHeight)/float2(Width,Height)),uint2(GuideWidth,GuideHeight)-1),0)).xy*float2(ScaleX,ScaleY);
    float2 q=id.xy+.5+mv; float4 old=0;
    bool valid=Inside(q);
    if(valid) {
        old=Sample(B,q);
        // Any invalid bilinear contributor invalidates the chain. A later
        // valid depth must never revive a point that was occluded on the way.
        valid=old.z>.9999 && DepthMatches(GuideDepth(C,id.xy+.5),GuideDepth(D,q));
    }
    Out[id.xy]=valid?float4(mv+old.xy,1,0):float4(0,0,0,0);
}
[numthreads(8,8,1)] void Compose(uint3 id:SV_DispatchThreadID) {
    if(id.x>=Width || id.y>=Height)return;
    float4 now=A.Load(int3(id.xy,0));
    if(!UseResult || Age>MaxAge){Out[id.xy]=now;return;}
    float4 flow=B.Load(int3(id.xy,0)); float2 q=id.xy+.5+flow.xy;
    bool valid=flow.z>.9999 && Inside(q);
    float3 delta=0;
    if(valid) {
        float4 anchor=Sample(C,q), enhanced=Sample(D,q);
        float scale=max(max(max(abs(now.r),abs(now.g)),abs(now.b)),.05);
        valid=DepthMatches(GuideDepth(E,id.xy+.5),GuideDepth(F,q)) &&
            all(isfinite(anchor.rgb)) && all(isfinite(enhanced.rgb)) &&
            all(abs(now.rgb-anchor.rgb)<=ColorTolerance*scale);
        if(valid)delta=enhanced.rgb-anchor.rgb;
    }
    float3 value=now.rgb+delta;
    if(!all(isfinite(value)))value=now.rgb;
    // Reapply limits against THIS frame, preserving negative producer channels
    // and alpha. The anchor's bounded answer is not a bound for current colour.
    if(valid) {
        const float3 weights=float3(.2126,.7152,.0722);
        float y=dot(max(value,0),weights), limit=max(dot(max(now.rgb,0),weights),0)*MaxRatio;
        if(y>limit)value=max(value,0)*limit/max(y,1e-10);
        value=min(value,65504);
        value=float3(now.r<0?now.r:value.r,now.g<0?now.g:value.g,now.b<0?now.b:value.b);
    }
    Out[id.xy]=float4(value,now.a);
}
)";
