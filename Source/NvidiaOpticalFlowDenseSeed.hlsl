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

float4 LoadFrame(Texture2D<float4> frame, int2 pixel)
{
    pixel = clamp(pixel, int2(0, 0), int2(FrameSize) - 1);
    return frame.Load(int3(pixel, 0));
}

float4 SampleFrame(Texture2D<float4> frame, float2 pixel)
{
    pixel = clamp(pixel, float2(0.0, 0.0), float2(FrameSize) - 1.0);
    int2 base = int2(floor(pixel));
    float2 f = frac(pixel);

    float4 v00 = LoadFrame(frame, base);
    float4 v10 = LoadFrame(frame, base + int2(1, 0));
    float4 v01 = LoadFrame(frame, base + int2(0, 1));
    float4 v11 = LoadFrame(frame, base + int2(1, 1));
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

bool ForwardMidpointNearlyRigid(int2 cell)
{
    float inverseSpacing = 0.5 / max(GridSize, 1.0e-6);
    float2 dx = (
        LoadFlow(ForwardFlowBtoA, cell + int2(1, 0)) -
        LoadFlow(ForwardFlowBtoA, cell - int2(1, 0))) * inverseSpacing;
    float2 dy = (
        LoadFlow(ForwardFlowBtoA, cell + int2(0, 1)) -
        LoadFlow(ForwardFlowBtoA, cell - int2(0, 1))) * inverseSpacing;

    // B -> midpoint at t=0.5 is x + 0.5*F(x). Only salvage raw forward
    // flow when that local map is very close to rigid; this intentionally uses
    // a much tighter condition than the general topology rejection in the warp.
    float2 columnX = float2(1.0 + 0.5 * dx.x, 0.5 * dx.y);
    float2 columnY = float2(0.5 * dy.x, 1.0 + 0.5 * dy.y);
    float determinant = columnX.x * columnY.y - columnX.y * columnY.x;
    float frobeniusSq = dot(columnX, columnX) + dot(columnY, columnY);
    float discriminant = sqrt(max(
        frobeniusSq * frobeniusSq - 4.0 * determinant * determinant, 0.0));
    float sigmaMax = sqrt(max(0.5 * (frobeniusSq + discriminant), 0.0));
    float sigmaMin = sqrt(max(0.5 * (frobeniusSq - discriminant), 0.0));

    return determinant > 0.75 && determinant < 1.25
        && sigmaMin > 0.75 && sigmaMax < 1.25;
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

    bool forwardSeedValid = bToAError <= ConsistencyThreshold;
    bool backwardSeedValid = aToBError <= ConsistencyThreshold;
    bool neitherConsistencyValid = !forwardSeedValid && !backwardSeedValid;

    // Extreme motion blur frequently breaks the round-trip test even when the
    // raw B->A vector still lands on the correct blurred structure. Salvage only
    // a deliberately strict subset: the B->A endpoint must match extremely well
    // and its local midpoint map must remain close to rigid. The first live test
    // uses a hard 0.025 RGB-MAD ceiling and 0.75..1.25 singular-value/determinant
    // window so the truly ambiguous core keeps the safe temporal fallback.
    float2 previousPixel = pixel + bToA;
    bool previousPixelInBounds = all(previousPixel >= float2(0.0, 0.0))
        && all(previousPixel <= float2(FrameSize) - 1.0);
    float bToAPhotoError = 1.0;
    if (neitherConsistencyValid && previousPixelInBounds) {
        float3 previousRgb = SampleFrame(PreviousFrame, previousPixel).rgb;
        float3 nextRgb = SampleFrame(NextFrame, pixel).rgb;
        bToAPhotoError = dot(
            abs(previousRgb - nextRgb),
            float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
    }

    bool forwardSalvage = neitherConsistencyValid
        && previousPixelInBounds
        && bToAPhotoError <= 0.025
        && ForwardMidpointNearlyRigid(cell);
    bool unsupported = neitherConsistencyValid && !forwardSalvage;

    // Build a local-repair motion candidate in a common A->B orientation.
    // A salvaged B->A seed stays on that same forward solution so the repair
    // pass cannot immediately switch it back to the less credible direction.
    bool useAtoB = !forwardSalvage && aToBError <= bToAError;
    float2 repairMotion = useAtoB ? aToB : -bToA;
    float repairError = min(aToBError, bToAError);
    float repairConfidence = exp(-min(repairError, 80.0) / 10.0);
    if (forwardSalvage) {
        repairConfidence = max(
            repairConfidence,
            1.0 - smoothstep(0.010, 0.040, bToAPhotoError));
    }

    // Pack two exact small-integer flags into W for the repair pass:
    // bit 0 = old catastrophic-region mask; bit 1 = neither NVOF direction
    // has a trustworthy seed after the conservative raw-forward salvage test.
    float repairFlags = (catastrophic ? 1.0 : 0.0) + (unsupported ? 2.0 : 0.0);
    RepairCandidate[id.xy] = float4(
        repairMotion, repairConfidence, repairFlags);

    // Preserve every consistency-valid native B->A seed. If only A->B passes,
    // keep the asymmetric -A->B backfill. If neither passes, admit raw B->A only
    // through the strict photo + near-rigid salvage gate above.
    bool useBackwardSeed = !forwardSeedValid && backwardSeedValid;
    SeedMap[id.xy] = (forwardSeedValid || backwardSeedValid || forwardSalvage)
        ? PackSeed(id.xy, useBackwardSeed)
        : InvalidSeed;
}
