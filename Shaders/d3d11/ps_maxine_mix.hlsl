Texture2D baseline : register(t0);
Texture2D enhanced : register(t1);
SamplerState samp : register(s0);
cbuffer PS_AMOUNT : register(b1)
{
    float2 enhancedOffset; // destination pixel to enhanced texture pixel
    float amount;
};
struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};
float4 main(PS_INPUT input) : SV_Target
{
    int2 enhancedPixel = int2(input.Pos.xy + enhancedOffset);
    return float4(lerp(baseline.Sample(samp, input.Tex).rgb,
        enhanced.Load(int3(enhancedPixel, 0)).rgb, amount), 1);
}
