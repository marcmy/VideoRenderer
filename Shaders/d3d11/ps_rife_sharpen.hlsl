Texture2D frame : register(t0);

cbuffer PS_BOUNDS : register(b0)
{
    int2 firstPixel;
    int2 lastPixel;
};

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float LumaAt(int2 pixel)
{
    return Luma(frame.Load(int3(clamp(pixel, firstPixel, lastPixel), 0)).rgb);
}

float4 main(float4 position : SV_POSITION) : SV_Target
{
    const int2 pixel = int2(position.xy);
    const float4 center = frame.Load(int3(pixel, 0));
    const float luma = Luma(center.rgb);
    const float left = LumaAt(pixel + int2(-1, 0));
    const float right = LumaAt(pixel + int2(1, 0));
    const float above = LumaAt(pixel + int2(0, -1));
    const float below = LumaAt(pixel + int2(0, 1));

    const float detail = (luma - (left + right + above + below) * 0.25) * 0.25;
    const float correction = sign(detail) * min(max(abs(detail) - 1.0 / 255.0, 0.0), 5.0 / 255.0);
    // Bound the luma adjustment by nearby samples to avoid edge overshoot.
    const float low = min(luma, min(min(left, right), min(above, below)));
    const float high = max(luma, max(max(left, right), max(above, below)));
    const float delta = clamp(luma + correction, low, high) - luma;
    // An equal offset in all three channels keeps the chroma differences.
    const float dark = min(center.r, min(center.g, center.b));
    const float bright = max(center.r, max(center.g, center.b));
    return float4(center.rgb + clamp(delta, -dark, 1.0 - bright), center.a);
}
