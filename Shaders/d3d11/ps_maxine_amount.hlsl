// Reuse the production Jinc2m implementation rather than a second resampler.
#define main Jinc2mSample
#include "../examples/ps_resize_onepass_jinc2.hlsl"
#undef main

Texture2D enhanced : register(t1);
cbuffer PS_AMOUNT : register(b1)
{
    float2 enhancedOffset; // destination pixel to enhanced texture pixel
    float amount;
};

float4 main(PS_INPUT input) : SV_Target
{
    int2 enhancedPixel = int2(input.Pos.xy + enhancedOffset);
    // Clamp before mixing, matching the ordinary UNORM Jinc2m baseline.
    float3 baseline = saturate(Jinc2mSample(input).rgb);
    return float4(lerp(baseline, enhanced.Load(int3(enhancedPixel, 0)).rgb, amount), 1);
}
