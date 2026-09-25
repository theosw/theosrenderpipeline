#pragma once

namespace TheosRenderPipeline::SourceDLSSG
{
inline constexpr char kNeuralComposeShader[] = R"(
Texture2D<float4> scene : register(t0);
Texture2D<float4> ui : register(t1);
RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
    uint w,h; output.GetDimensions(w,h); if (p.x >= w || p.y >= h) return;
    float4 s = scene.Load(int3(p.xy,0)), u = ui.Load(int3(p.xy,0));
    output[p.xy] = float4(s.rgb * (1.0 - u.a) + u.rgb, max(s.a, u.a));
})";
}
