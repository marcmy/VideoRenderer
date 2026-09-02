// Experimental post-resize sharpening pass for evaluating Maxine VSR output.
// Load as a post-resize/post-scale shader so sharpening happens after the
// Maxine result has been resized to the actual player output.
//
// Medium strength: the proposed default candidate if the A/B test shows that
// final presentation scaling is responsible for the perceived softness.

Texture2D<float4> tex : register(t0);
SamplerState samp : register(s0);

static const float SHARPNESS = 0.45;
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

float4 main(float4 pos : SV_Position, float2 coord : TEXCOORD0) : SV_Target
{
    uint width, height;
    tex.GetDimensions(width, height);
    const float2 px = 1.0 / float2(width, height);

    const float4 center = tex.Sample(samp, coord);
    const float3 north = tex.Sample(samp, coord + float2(0.0, -px.y)).rgb;
    const float3 south = tex.Sample(samp, coord + float2(0.0,  px.y)).rgb;
    const float3 west  = tex.Sample(samp, coord + float2(-px.x, 0.0)).rgb;
    const float3 east  = tex.Sample(samp, coord + float2( px.x, 0.0)).rgb;

    const float yc = dot(center.rgb, LUMA);
    const float yn = dot(north, LUMA);
    const float ys = dot(south, LUMA);
    const float yw = dot(west,  LUMA);
    const float ye = dot(east,  LUMA);

    const float blurY = (yn + ys + yw + ye) * 0.25;
    const float localMin = min(yc, min(min(yn, ys), min(yw, ye)));
    const float localMax = max(yc, max(max(yn, ys), max(yw, ye)));
    const float localRange = localMax - localMin;

    // Back off on strong contrast edges, where aggressive high-pass sharpening
    // is most likely to create visible ringing. Fine texture keeps full strength.
    const float edgeAttenuation = lerp(1.0, 0.55, saturate(localRange * 4.0));
    const float detail = (yc - blurY) * SHARPNESS * edgeAttenuation;

    return float4(saturate(center.rgb + detail.xxx), center.a);
}
