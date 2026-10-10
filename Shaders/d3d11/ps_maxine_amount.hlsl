// Reuse the production Jinc2m implementation rather than a second resampler.
#define main Jinc2mSample
#include "../examples/ps_resize_onepass_jinc2.hlsl"
#undef main

Texture2D enhanced : register(t1);
cbuffer PS_AMOUNT : register(b1)
{
    float4 destination; // left, top, width, height in render-target pixels
    float4 amount;      // x: Maxine fraction, remaining fields reserved
};

float4 main(PS_INPUT input) : SV_Target
{
    float2 uv = (input.Pos.xy - destination.xy) / destination.zw;
    // Clamp before mixing, matching the ordinary UNORM Jinc2m baseline.
    float3 baseline = saturate(Jinc2mSample(input).rgb);
    return float4(lerp(baseline, enhanced.Sample(samp, uv).rgb, amount.x), 1);
}
