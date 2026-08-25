Texture2D<int2> ForwardFlowBtoA : register(t0);
Texture2D<int2> BackwardFlowAtoB : register(t1);
Texture2D<float4> PreviousFrame : register(t2);
Texture2D<float4> NextFrame : register(t3);
RWTexture2D<uint> SeedMap : register(u0);
RWTexture2D<uint> UnsafeCellCount : register(u1);
RWTexture2D<uint> UnsafeCellMap : register(u2);
RWTexture2D<float4> RepairCandidate : register(u3);

cbuffer SeedParameters : register(b0)
{
    uint2 FlowSize;
    float GridSize;
    float ConsistencyThreshold;

    float MotionThreshold;
    uint2 FrameSize;
    float CutHistogramThreshold;

    float CutCorrelationThreshold;
    float CutMadThreshold;
    float2 Padding;
};

static const uint InvalidSeed = 0xffffffffu;
static const uint SceneCutBit = 0x80000000u;
static const uint CutSampleWidth = 32u;
static const uint CutSampleHeight = 18u;
static const uint CutSampleCount = CutSampleWidth * CutSampleHeight;

float2 LoadFlow(Texture2D<int2> flowTexture, int2 cell)
{
    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);
    return float2(flowTexture.Load(int3(cell, 0))) / 32.0;
}

float2 SampleFlow(Texture2D<int2> flowTexture, float2 pixel)
{
    float2 grid = pixel / GridSize;
    int2 base = int2(floor(grid));
    float2 f = frac(grid);

    float2 v00 = LoadFlow(flowTexture, base);
    float2 v10 = LoadFlow(flowTexture, base + int2(1, 0));
    float2 v01 = LoadFlow(flowTexture, base + int2(0, 1));
    float2 v11 = LoadFlow(flowTexture, base + int2(1, 1));
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

static const uint BackwardSeedBit = 0x80000000u;

uint PackSeed(uint2 cell, bool useBackwardSeed)
{
    uint packed = (cell.y << 16) | (cell.x & 0xffffu);
    return useBackwardSeed ? (packed | BackwardSeedBit) : packed;
}

uint SampleIntensity(Texture2D<float4> frame, uint2 pixel)
{
    float3 rgb = saturate(frame.Load(int3(pixel, 0)).rgb);
    return (uint)round((rgb.r + rgb.g + rgb.b) * (255.0 / 3.0));
}

bool DetectSceneCut()
{
    uint sumA = 0u;
    uint sumB = 0u;
    uint sumAA = 0u;
    uint sumBB = 0u;
    uint sumAB = 0u;
    uint sumAbs = 0u;

    [loop]
    for (uint sy = 0u; sy < CutSampleHeight; ++sy) {
        [loop]
        for (uint sx = 0u; sx < CutSampleWidth; ++sx) {
            uint2 pixel = uint2(
                min(((2u * sx + 1u) * FrameSize.x) / (2u * CutSampleWidth), FrameSize.x - 1u),
                min(((2u * sy + 1u) * FrameSize.y) / (2u * CutSampleHeight), FrameSize.y - 1u));
            uint a = SampleIntensity(PreviousFrame, pixel);
            uint b = SampleIntensity(NextFrame, pixel);
            sumA += a;
            sumB += b;
            sumAA += a * a;
            sumBB += b * b;
            sumAB += a * b;
            sumAbs += a > b ? a - b : b - a;
        }
    }

    float n = float(CutSampleCount);
    float mad = float(sumAbs) / (n * 255.0);
    float covariance = n * float(sumAB) - float(sumA) * float(sumB);
    float varianceA = max(n * float(sumAA) - float(sumA) * float(sumA), 0.0);
    float varianceB = max(n * float(sumBB) - float(sumB) * float(sumB), 0.0);
    float correlation = covariance / sqrt(max(varianceA * varianceB, 1.0));

    // The expanded Chamber-of-Mazarbul capture corpus showed that dark cuts
    // can retain nearly the same coarse intensity histogram. Spatial
    // decorrelation plus substantial absolute image change separated all
    // captured hard cuts from the same-shot fast-motion examples cleanly.
    return correlation < CutCorrelationThreshold
        && mad > CutMadThreshold;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (all(id.xy == uint2(0, 0)) && DetectSceneCut()) {
        InterlockedOr(UnsafeCellCount[uint2(0, 0)], SceneCutBit);
    }

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
    UnsafeCellMap[id.xy] = catastrophic ? 1u : 0u;
    if (catastrophic) {
        InterlockedAdd(UnsafeCellCount[uint2(0, 0)], 1u);
    }

    // Build a local-repair motion candidate in a common A->B orientation.
    // Occlusions are strongly asymmetric: keep the direction whose own
    // round-trip check is more trustworthy instead of discarding both.
    bool useAtoB = aToBError <= bToAError;
    float2 repairMotion = useAtoB ? aToB : -bToA;
    float repairError = min(aToBError, bToAError);
    float repairConfidence = exp(-min(repairError, 80.0) / 10.0);
    RepairCandidate[id.xy] = float4(
        repairMotion, repairConfidence, catastrophic ? 1.0 : 0.0);

    // Preserve every trustworthy native B->A seed. If that direction fails
    // its own round-trip check but A->B is still trustworthy, use the negated
    // A->B vector as an asymmetric backfill seed instead of creating a large
    // JFA hole. When both pass, prefer native B->A to preserve existing behavior.
    bool forwardSeedValid = bToAError <= ConsistencyThreshold;
    bool backwardSeedValid = aToBError <= ConsistencyThreshold;
    bool useBackwardSeed = !forwardSeedValid && backwardSeedValid;
    SeedMap[id.xy] = (forwardSeedValid || backwardSeedValid)
        ? PackSeed(id.xy, useBackwardSeed)
        : InvalidSeed;
}
