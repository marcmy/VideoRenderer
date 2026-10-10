Texture2D baseline : register(t0);
Texture2D enhanced : register(t1);
SamplerState samp : register(s0);
cbuffer PS_AMOUNT : register(b1)
{
    float4 destination;
    float4 amount;
};
struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};
float4 main(PS_INPUT input) : SV_Target
{
    float2 uv = (input.Pos.xy - destination.xy) / destination.zw;
    return float4(lerp(baseline.Sample(samp, input.Tex).rgb,
        enhanced.Sample(samp, uv).rgb, amount.x), 1);
}
