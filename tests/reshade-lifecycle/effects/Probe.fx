texture World : COLOR;
sampler WorldSampler { Texture = World; };
void VS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = tex2D(WorldSampler, uv);
    color.r += 0.125;
    return color;
}
technique Probe { pass { VertexShader = VS; PixelShader = PS; } }
