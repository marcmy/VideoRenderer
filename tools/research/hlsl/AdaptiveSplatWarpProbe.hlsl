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

static const uint SceneCutBit = 0x80000000u;
static const float SupportedMidpointEpsilon = 1.0e-4;

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

float4 ChannelMedian(float4 a, float4 b, float4 c)
{
    float4 lo = min(a, min(b, c));
    float4 hi = max(a, max(b, c));
    return a + b + c - lo - hi;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FrameSize)) return;

    float2 pixel = float2(id.xy);
    float2 frameUv = PixelUv(pixel);
    float4 previous = PreviousFrame.SampleLevel(LinearClamp, frameUv, 0.0);
    float4 next = NextFrame.SampleLevel(LinearClamp, frameUv, 0.0);

    if ((PackedQuality.Load(int3(0, 0, 0)) & SceneCutBit) != 0u) {
        OutputFrame[id.xy] = previous;
        return;
    }

    // Stand-in golden reconstruction for compile/resource-layout proof. The
    // production patch will append the adaptive median to the existing golden
    // Warp result rather than replace the golden logic with this probe.
    float2 dense = DenseFlow.SampleLevel(LinearClamp, frameUv, 0.0);
    float4 denseCandidate = NextFrame.SampleLevel(
        LinearClamp, PixelUv(pixel - 0.5 * dense), 0.0);
    float4 repair = RepairField.SampleLevel(LinearClamp, CoarseUv(pixel), 0.0);
    float4 repairCandidate = PreviousFrame.SampleLevel(
        LinearClamp, PixelUv(pixel - 0.5 * repair.xy), 0.0);
    float4 golden = lerp(denseCandidate, repairCandidate, saturate(repair.z));

    // Build #2 adaptive path is midpoint-only by overflow proof. Any other
    // phase leaves the existing golden reconstruction untouched.
    if (abs(MidpointTime - 0.5) > SupportedMidpointEpsilon) {
        OutputFrame[id.xy] = golden;
        return;
    }

    float2 uv = CoarseUv(pixel);
    float4 mapA = ResolvedSplat.SampleLevel(LinearClamp, float3(uv, 0.0), 0.0);
    float4 mapB = ResolvedSplat.SampleLevel(LinearClamp, float3(uv, 1.0), 0.0);
    float4 temporal = 0.5 * (previous + next);

    float4 warpedA = PreviousFrame.SampleLevel(
        LinearClamp, PixelUv(pixel + mapA.xy), 0.0);
    float4 warpedB = NextFrame.SampleLevel(
        LinearClamp, PixelUv(pixel + mapB.xy), 0.0);

    float wa = pow(max(mapA.z, 1.0e-8), 3.0);
    float wb = pow(max(mapB.z, 1.0e-8), 3.0);
    float denom = wa + wb;
    float4 alternate = denom > 1.0e-7
        ? (warpedA * wa + warpedB * wb) / denom
        : temporal;

    float4 robust = ChannelMedian(golden, alternate, temporal);
    float localRisk = SmoothstepScalar(1200.0, 2400.0, mapA.w)
        * SmoothstepScalar(0.03, 0.22, max(mapA.z, mapB.z));
    float alpha = min(localRisk, 0.60) * FieldAuthority();
    OutputFrame[id.xy] = lerp(golden, robust, alpha);
}
