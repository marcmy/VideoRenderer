Texture2D<float4> PreviousFrame : register(t0);
Texture2D<float4> NextFrame : register(t1);
Texture2D<float2> DenseFlow : register(t2);
Texture2D<uint> PackedQuality : register(t3);
Texture2D<float4> RepairField : register(t4);
Texture2D<uint> PackedRegionStats : register(t5);
Texture2DArray<float4> ResolvedSplat : register(t6);

SamplerState LinearClamp : register(s0);
RWTexture2D<float4> OutputFrame : register(u0);

cbuffer WarpProbeParameters : register(b0)
{
    uint2 FrameSize;
    uint2 FlowSize;
    float MidpointTime;
    float GridSize;
    float2 Padding;
};

float2 PixelUv(float2 pixel)
{
    return (pixel + 0.5) / float2(FrameSize);
}

float2 CoarseUv(float2 pixel)
{
    float2 coarse = pixel / max(GridSize, 1.0e-6);
    return (coarse + 0.5) / float2(FlowSize);
}

float SmoothstepScalar(float lo, float hi, float v)
{
    float x = saturate((v - lo) / max(hi - lo, 1.0e-6));
    return x * x * (3.0 - 2.0 * x);
}

float FieldAuthority()
{
    uint borderX = max(1u, (uint)(float(FlowSize.x) * 0.04));
    uint borderY = max(1u, (uint)(float(FlowSize.y) * 0.04));
    uint interiorW = FlowSize.x > 2u * borderX ? FlowSize.x - 2u * borderX : 1u;
    uint interiorH = FlowSize.y > 2u * borderY ? FlowSize.y - 2u * borderY : 1u;
    uint interiorCount = max(1u, interiorW * interiorH);
    uint badCount = PackedRegionStats.Load(int3(0, 0, 0)) >> 6u;
    return SmoothstepScalar(15.0, 25.0,
        100.0 * float(badCount) / float(interiorCount));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FrameSize)) return;

    float2 pixel = float2(id.xy);
    float2 uv = CoarseUv(pixel);
    float4 mapA = ResolvedSplat.SampleLevel(LinearClamp, float3(uv, 0.0), 0.0);
    float4 mapB = ResolvedSplat.SampleLevel(LinearClamp, float3(uv, 1.0), 0.0);

    float4 temporal = lerp(
        PreviousFrame.SampleLevel(LinearClamp, PixelUv(pixel), 0.0),
        NextFrame.SampleLevel(LinearClamp, PixelUv(pixel), 0.0),
        MidpointTime);

    float4 warpedA = PreviousFrame.SampleLevel(
        LinearClamp, PixelUv(pixel + mapA.xy), 0.0);
    float4 warpedB = NextFrame.SampleLevel(
        LinearClamp, PixelUv(pixel + mapB.xy), 0.0);

    float t = saturate(MidpointTime);
    float wa = (1.0 - t) * pow(max(mapA.z, 1.0e-8), 3.0);
    float wb = t * pow(max(mapB.z, 1.0e-8), 3.0);
    float denom = wa + wb;
    float4 alternate = denom > 1.0e-7
        ? (warpedA * wa + warpedB * wb) / denom
        : temporal;

    // Compile probe only: use temporal as the stand-in 'golden' hypothesis.
    float4 robust = temporal + alternate - temporal;
    float localRisk = SmoothstepScalar(1200.0, 2400.0, mapA.w)
        * SmoothstepScalar(0.03, 0.22, max(mapA.z, mapB.z));
    float phaseEnvelope = 4.0 * t * (1.0 - t);
    float alpha = min(localRisk, 0.60) * FieldAuthority() * phaseEnvelope;
    OutputFrame[id.xy] = lerp(temporal, robust, alpha);
}
