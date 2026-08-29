Texture2D<int2> ForwardFlowBtoA : register(t0);
Texture2D<int2> BackwardFlowAtoB : register(t1);
Texture2D<uint> PackedCellMetadata : register(t2);
Texture2D<uint> PackedQuality : register(t3);
Texture2D<uint> PackedRegionStats : register(t4);

RWStructuredBuffer<int> SplatAccum : register(u0);

cbuffer SplatParameters : register(b0)
{
    uint2 FlowSize;
    float GridSize;
    float MidpointTime;
};

static const uint SceneCutBit = 0x80000000u;
static const uint FieldCountShift = 6u;
static const float WeightScale = 60.0;
static const float SupportedMidpointEpsilon = 1.0e-4;

float2 LoadFlow(Texture2D<int2> tex, uint2 cell)
{
    return float2(tex.Load(int3(cell, 0))) / 32.0;
}

float SmoothstepScalar(float lo, float hi, float v)
{
    float x = saturate((v - lo) / max(hi - lo, 1.0e-6));
    return x * x * (3.0 - 2.0 * x);
}

float FieldAuthority()
{
    if ((PackedQuality.Load(int3(0, 0, 0)) & SceneCutBit) != 0u) {
        return 0.0;
    }

    uint borderX = max(1u, (uint)(float(FlowSize.x) * 0.04));
    uint borderY = max(1u, (uint)(float(FlowSize.y) * 0.04));
    uint interiorW = FlowSize.x > 2u * borderX ? FlowSize.x - 2u * borderX : 1u;
    uint interiorH = FlowSize.y > 2u * borderY ? FlowSize.y - 2u * borderY : 1u;
    uint interiorCount = max(1u, interiorW * interiorH);
    uint badCount = PackedRegionStats.Load(int3(0, 0, 0)) >> FieldCountShift;
    float badPercent = 100.0 * float(badCount) / float(interiorCount);
    return SmoothstepScalar(15.0, 25.0, badPercent);
}

void ScatterDirection(uint2 sourceCell, float2 flow, uint errorQ, uint qQ,
    bool endpointInBounds, uint accumulatorOffset)
{
    if (!endpointInBounds || errorQ > 10u) {
        return;
    }

    // Build #2 is intentionally midpoint-only. The signed-32-bit accumulator
    // overflow proof assumes a 0.5 projection scale. The current renderer
    // scheduler requests exactly t=0.5; any future arbitrary-phase scheduler
    // must use a separately proven accumulator scale/representation.
    static const float Phase = 0.5;

    float errorPx = float(errorQ * 2u);
    float q = float(qQ * 8u);
    float confidence = exp(-min(errorPx, 40.0) / 8.0)
        * exp(-min(q, 8000.0) / 1200.0);

    float2 targetCell = float2(sourceCell) + Phase * flow / GridSize;
    int2 baseCell = int2(floor(targetCell));
    float2 fracCell = frac(targetCell);
    float2 inverseDisplacement = -Phase * flow;

    [unroll]
    for (uint oy = 0u; oy < 2u; ++oy) {
        int targetY = baseCell.y + int(oy);
        if (targetY < 0 || targetY >= int(FlowSize.y)) continue;
        float wy = oy == 0u ? 1.0 - fracCell.y : fracCell.y;

        [unroll]
        for (uint ox = 0u; ox < 2u; ++ox) {
            int targetX = baseCell.x + int(ox);
            if (targetX < 0 || targetX >= int(FlowSize.x)) continue;
            float wx = ox == 0u ? 1.0 - fracCell.x : fracCell.x;
            int weightQ = (int)round(wx * wy * confidence * WeightScale);
            if (weightQ <= 0) continue;

            uint targetIndex = uint(targetY) * FlowSize.x + uint(targetX);
            uint base = targetIndex * 6u + accumulatorOffset;
            int weightedX = (int)round(inverseDisplacement.x * float(weightQ));
            int weightedY = (int)round(inverseDisplacement.y * float(weightQ));
            InterlockedAdd(SplatAccum[base + 0u], weightedX);
            InterlockedAdd(SplatAccum[base + 1u], weightedY);
            InterlockedAdd(SplatAccum[base + 2u], weightQ);
        }
    }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;

    // Golden-only behavior for any phase other than the proven midpoint path.
    if (abs(MidpointTime - 0.5) > SupportedMidpointEpsilon) return;
    if (FieldAuthority() <= 0.0) return;

    uint packed = PackedCellMetadata.Load(int3(id.xy, 0));
    uint qBtoA = packed & 0x3ffu;
    uint qAtoB = (packed >> 10u) & 0x3ffu;
    uint errBtoA = (packed >> 20u) & 0x0fu;
    uint errAtoB = (packed >> 24u) & 0x0fu;
    bool inBoundsBtoA = (packed & (1u << 28u)) != 0u;
    bool inBoundsAtoB = (packed & (1u << 29u)) != 0u;

    ScatterDirection(id.xy, LoadFlow(BackwardFlowAtoB, id.xy),
        errAtoB, qAtoB, inBoundsAtoB, 0u);
    ScatterDirection(id.xy, LoadFlow(ForwardFlowBtoA, id.xy),
        errBtoA, qBtoA, inBoundsBtoA, 3u);
}
