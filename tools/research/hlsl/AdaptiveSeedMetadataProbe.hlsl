Texture2D<int2> ForwardFlowBtoA : register(t0);
Texture2D<int2> BackwardFlowAtoB : register(t1);
Texture2D<float4> PreviousFrame : register(t2);
Texture2D<float4> NextFrame : register(t3);
SamplerState LinearClamp : register(s0);

RWTexture2D<uint> SeedMapProbe : register(u0);
RWTexture2D<uint> PackedQualityProbe : register(u1);
RWTexture2D<uint> PackedCellMetadata : register(u2);
RWTexture2D<float4> RepairCandidateProbe : register(u3);
RWTexture2D<uint> PackedRegionStats : register(u4);

cbuffer SeedProbeParameters : register(b0)
{
    uint2 FlowSize;
    float GridSize;
    float ConsistencyThreshold;
    uint2 FrameSize;
    float MotionThreshold;
    float Padding;
};

static const uint CatastrophicBit = 1u << 30u;
static const uint FieldCountIncrement = 1u << 6u;

float2 LoadFlow(Texture2D<int2> tex, int2 cell)
{
    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);
    return float2(tex.Load(int3(cell, 0))) / 32.0;
}

float2 SampleFlow(Texture2D<int2> tex, float2 pixel)
{
    float2 grid = pixel / GridSize;
    int2 base = int2(floor(grid));
    float2 f = frac(grid);
    float2 v00 = LoadFlow(tex, base);
    float2 v10 = LoadFlow(tex, base + int2(1, 0));
    float2 v01 = LoadFlow(tex, base + int2(0, 1));
    float2 v11 = LoadFlow(tex, base + int2(1, 1));
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float4 SampleFrame(Texture2D<float4> tex, float2 pixel)
{
    float2 uv = (pixel + 0.5) / float2(FrameSize);
    return tex.SampleLevel(LinearClamp, uv, 0.0);
}

uint QuantizeQ(float q)
{
    return min((uint)round(min(q, 8184.0) / 8.0), 1023u);
}

uint QuantizeError(float errorPx)
{
    return min((uint)ceil(max(errorPx, 0.0) / 2.0), 15u);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;

    int2 cell = int2(id.xy);
    float2 pixel = float2(cell) * GridSize;
    float2 bToA = LoadFlow(ForwardFlowBtoA, cell);
    float2 aToB = LoadFlow(BackwardFlowAtoB, cell);

    float bToAError = length(bToA + SampleFlow(BackwardFlowAtoB, pixel + bToA));
    float aToBError = length(aToB + SampleFlow(ForwardFlowBtoA, pixel + aToB));
    float consistency = max(bToAError, aToBError);
    float motion = max(length(bToA), length(aToB));
    bool catastrophic = motion > MotionThreshold && consistency > ConsistencyThreshold;

    float2 previousEndpoint = pixel + bToA;
    float2 nextEndpoint = pixel + aToB;
    bool inBoundsBtoA = all(previousEndpoint >= 0.0)
        && all(previousEndpoint <= float2(FrameSize) - 1.0);
    bool inBoundsAtoB = all(nextEndpoint >= 0.0)
        && all(nextEndpoint <= float2(FrameSize) - 1.0);

    float yB = Luma(NextFrame.Load(int3(int2(pixel), 0)).rgb);
    float yA = Luma(PreviousFrame.Load(int3(int2(pixel), 0)).rgb);
    float qBtoA = 0.0;
    float qAtoB = 0.0;
    if (inBoundsBtoA) {
        qBtoA = 16.0 * 255.0
            * abs(yB - Luma(SampleFrame(PreviousFrame, previousEndpoint).rgb))
            / max(yB, 1.0 / 255.0);
    }
    if (inBoundsAtoB) {
        qAtoB = 16.0 * 255.0
            * abs(yA - Luma(SampleFrame(NextFrame, nextEndpoint).rgb))
            / max(yA, 1.0 / 255.0);
    }
    float qCombined = 0.5 * (qBtoA + qAtoB);

    uint packed = QuantizeQ(qBtoA)
        | (QuantizeQ(qAtoB) << 10u)
        | (QuantizeError(bToAError) << 20u)
        | (QuantizeError(aToBError) << 24u)
        | (inBoundsBtoA ? (1u << 28u) : 0u)
        | (inBoundsAtoB ? (1u << 29u) : 0u)
        | (catastrophic ? CatastrophicBit : 0u);
    PackedCellMetadata[id.xy] = packed;

    if (catastrophic) {
        InterlockedAdd(PackedQualityProbe[uint2(0, 0)], 1u);
    }

    uint borderX = max(1u, (uint)(float(FlowSize.x) * 0.04));
    uint borderY = max(1u, (uint)(float(FlowSize.y) * 0.04));
    bool interior = id.x >= borderX && id.x < FlowSize.x - borderX
        && id.y >= borderY && id.y < FlowSize.y - borderY;
    if (interior && qCombined >= 1600.0) {
        InterlockedAdd(PackedRegionStats[uint2(0, 0)], FieldCountIncrement);
    }

    // Dummy writes keep the probe's intended five-UAV Seed binding shape live.
    SeedMapProbe[id.xy] = bToAError <= ConsistencyThreshold ? 1u : 0xffffffffu;
    RepairCandidateProbe[id.xy] = float4(aToB, exp(-min(aToBError, 80.0) / 10.0), catastrophic ? 1.0 : 0.0);
}
