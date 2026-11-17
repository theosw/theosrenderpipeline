#pragma once
namespace TheosRenderPipeline::SourceDLSSG {
inline constexpr char kVulkanNRShader[] = R"hlsl(
Texture2D<float4> colour : register(t0);
Texture2D<float2> motion : register(t1);
Texture2D<float> depth : register(t2);
RWTexture2D<float4> colourOut : register(u0);
RWTexture2D<float2> motionOut : register(u1);
RWTexture2D<float> depthOut : register(u2);
cbuffer Extents : register(b0) { uint width, height, guideWidth, guideHeight; };
[numthreads(8,8,1)] void Pack(uint3 p:SV_DispatchThreadID) {
    if(p.x<width && p.y<height) colourOut[p.xy]=colour.Load(int3(p.xy,0));
    if(p.x<guideWidth && p.y<guideHeight) {
        motionOut[p.xy]=motion.Load(int3(p.xy,0));
        depthOut[p.xy]=depth.Load(int3(p.xy,0));
    }
}
[numthreads(8,8,1)] void Unpack(uint3 p:SV_DispatchThreadID) {
    if(p.x<width && p.y<height) colourOut[p.xy]=colour.Load(int3(p.xy,0));
}
)hlsl";
}
