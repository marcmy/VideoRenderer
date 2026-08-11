from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

seed_hlsl = r'''Texture2D<int2> ForwardFlowBtoA : register(t0);
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
static const uint CutHistogramBins = 16u;
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

uint PackSeed(uint2 cell)
{
    return (cell.y << 16) | (cell.x & 0xffffu);
}

uint SampleIntensity(Texture2D<float4> frame, uint2 pixel)
{
    float3 rgb = saturate(frame.Load(int3(pixel, 0)).rgb);
    return (uint)round((rgb.r + rgb.g + rgb.b) * (255.0 / 3.0));
}

bool DetectSceneCut()
{
    uint histA[CutHistogramBins];
    uint histB[CutHistogramBins];
    [unroll]
    for (uint bin = 0u; bin < CutHistogramBins; ++bin) {
        histA[bin] = 0u;
        histB[bin] = 0u;
    }

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
            histA[min(a >> 4, CutHistogramBins - 1u)]++;
            histB[min(b >> 4, CutHistogramBins - 1u)]++;
            sumA += a;
            sumB += b;
            sumAA += a * a;
            sumBB += b * b;
            sumAB += a * b;
            sumAbs += a > b ? a - b : b - a;
        }
    }

    uint intersectionCount = 0u;
    [unroll]
    for (uint bin = 0u; bin < CutHistogramBins; ++bin) {
        intersectionCount += min(histA[bin], histB[bin]);
    }

    float n = float(CutSampleCount);
    float histogramIntersection = float(intersectionCount) / n;
    float mad = float(sumAbs) / (n * 255.0);
    float covariance = n * float(sumAB) - float(sumA) * float(sumB);
    float varianceA = max(n * float(sumAA) - float(sumA) * float(sumA), 0.0);
    float varianceB = max(n * float(sumBB) - float(sumB) * float(sumB), 0.0);
    float correlation = covariance / sqrt(max(varianceA * varianceB, 1.0));

    // Conservative three-way classifier derived from the captured LOTR set.
    // A true shot cut must simultaneously change the intensity distribution,
    // destroy spatial correlation, and have substantial absolute image change.
    return histogramIntersection < CutHistogramThreshold
        && correlation < CutCorrelationThreshold
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

    SeedMap[id.xy] = consistency <= ConsistencyThreshold
        ? PackSeed(id.xy)
        : InvalidSeed;
}
'''

warp_hlsl = r'''Texture2D<float4> PreviousFrame : register(t0);
Texture2D<float4> NextFrame : register(t1);
Texture2D<float2> DenseFlow : register(t2);
Texture2D<uint> UnsafeCellCount : register(t3);
Texture2D<float4> RepairField : register(t4);

SamplerState LinearClamp : register(s0);
RWTexture2D<float4> OutputFrame : register(u0);

cbuffer WarpParameters : register(b0)
{
    uint2 FrameSize;
    uint FlowCellCount;
    float RepeatBadFraction;
    float MidpointTime;
    float RepairGridSize;
    float2 Padding;
};

static const uint SceneCutBit = 0x80000000u;
static const uint UnsafeCountMask = 0x7fffffffu;

float2 PixelToUv(float2 pixel)
{
    return (pixel + 0.5) / float2(FrameSize);
}

float2 SampleDenseFlow(float2 pixel)
{
    return DenseFlow.SampleLevel(LinearClamp, PixelToUv(pixel), 0.0);
}

float4 SampleRepair(float2 pixel)
{
    uint repairWidth, repairHeight;
    RepairField.GetDimensions(repairWidth, repairHeight);
    float2 repairGrid = pixel / max(RepairGridSize, 1.0e-6);
    float2 repairUv = (repairGrid + 0.5) / float2(repairWidth, repairHeight);
    return RepairField.SampleLevel(LinearClamp, repairUv, 0.0);
}

float4 SampleFrame(Texture2D<float4> frame, float2 pixel)
{
    return frame.SampleLevel(LinearClamp, PixelToUv(pixel), 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FrameSize)) return;

    uint packedQuality = UnsafeCellCount.Load(int3(0, 0, 0));
    bool sceneCut = (packedQuality & SceneCutBit) != 0u;
    uint unsafeCount = packedQuality & UnsafeCountMask;
    float unsafeFraction = float(unsafeCount) / max(1.0, float(FlowCellCount));
    float2 target = float2(id.xy);

    // Optical flow has no meaningful solution across an actual hard cut.
    // Preserve source timing by holding the previous shot until the real B
    // frame's timestamp rather than morphing unrelated camera angles.
    if (sceneCut) {
        OutputFrame[id.xy] = SampleFrame(PreviousFrame, target);
        return;
    }

    float2 source = target;
    float towardPrevious = 1.0 - MidpointTime;

    [unroll]
    for (int iteration = 0; iteration < 6; ++iteration) {
        float2 flow = SampleDenseFlow(source);
        source = target - towardPrevious * flow;
    }

    float4 current = SampleFrame(NextFrame, source);

    // Local occlusion repair. The mask follows both the source coordinate
    // selected by the dense B-side warp and the target coordinate. The coherent
    // A->B repair motion is evaluated in target coordinates.
    float repairMask = max(SampleRepair(source).z, SampleRepair(target).z);
    if (repairMask > 1.0e-4) {
        float2 repairMotion = SampleRepair(target).xy;
        float2 previousSource = target - MidpointTime * repairMotion;
        float2 nextSource = target + (1.0 - MidpointTime) * repairMotion;
        float4 safePrevious = SampleFrame(PreviousFrame, previousSource);
        float4 safeNext = SampleFrame(NextFrame, nextSource);
        float4 safe = lerp(safePrevious, safeNext, MidpointTime);
        current = lerp(current, safe, saturate(repairMask));
    }

    // When the flow field collapses over a large fraction of a *same shot*,
    // progressively prefer an unwarped temporal midpoint over melted geometry.
    // Unlike the old whole-frame repeat gate, this keeps a distinct midpoint
    // and therefore preserves the doubled presentation cadence.
    float safetyBlend = smoothstep(0.25, 0.40, unsafeFraction);
    if (safetyBlend > 1.0e-4) {
        float4 temporalMidpoint = lerp(
            SampleFrame(PreviousFrame, target),
            SampleFrame(NextFrame, target),
            MidpointTime);
        current = lerp(current, temporalMidpoint, safetyBlend);
    }

    OutputFrame[id.xy] = current;
}
'''

(ROOT / 'Source/NvidiaOpticalFlowDenseSeed.hlsl').write_text(seed_hlsl, encoding='utf-8', newline='\n')
(ROOT / 'Source/NvidiaOpticalFlowDenseWarp.hlsl').write_text(warp_hlsl, encoding='utf-8', newline='\n')

header_path = ROOT / 'Source/NvidiaOpticalFlowDenseSynthesizer.h'
header = header_path.read_text(encoding='utf-8')
start = header.index('    struct SeedParameters {')
end = header.index('    struct RepairParameters {')
new_seed_struct = '''    struct SeedParameters {
        UINT flowWidth;
        UINT flowHeight;
        float gridSize;
        float consistencyThreshold;
        float motionThreshold;
        UINT frameWidth;
        UINT frameHeight;
        float cutHistogramThreshold;
        float cutCorrelationThreshold;
        float cutMadThreshold;
        float padding[2];
    };
    static_assert(sizeof(SeedParameters) == 48);

'''
header = header[:start] + new_seed_struct + header[end:]
old_field = '    UINT m_lastUnsafeCount = 0;\n    UINT m_lastMaxLocalUnsafe = 0;'
new_field = '    UINT m_lastUnsafeCount = 0;\n    bool m_lastSceneCut = false;\n    UINT m_lastMaxLocalUnsafe = 0;'
if old_field not in header:
    raise RuntimeError('Expected telemetry fields not found in synthesizer header')
header = header.replace(old_field, new_field, 1)
header_path.write_text(header, encoding='utf-8', newline='\n')

cpp_path = ROOT / 'Source/NvidiaOpticalFlowDenseSynthesizer.cpp'
cpp = cpp_path.read_text(encoding='utf-8')

def replace_once(old, new, label):
    global cpp
    if cpp.count(old) != 1:
        raise RuntimeError(f'{label}: expected one match, found {cpp.count(old)}')
    cpp = cpp.replace(old, new, 1)

replace_once(
'''    m_telemetryWriteIndex = 0;
    m_lastUnsafeCount = 0;
    m_lastMaxLocalUnsafe = 0;
    m_haveTelemetry = false;''',
'''    m_telemetryWriteIndex = 0;
    m_lastUnsafeCount = 0;
    m_lastSceneCut = false;
    m_lastMaxLocalUnsafe = 0;
    m_haveTelemetry = false;''',
'reset telemetry')

replace_once(
'''    const UINT cellCount = m_flowWidth * m_flowHeight;
    const double badPercent = 100.0 * static_cast<double>(m_lastUnsafeCount) /
        std::max(1u, cellCount);
    return std::format(
        L"bad {:.1f}% ({}/{}), worst7x7 {}/49, would8={}, would18={}",
        badPercent, m_lastUnsafeCount, cellCount, m_lastMaxLocalUnsafe,
        m_lastMaxLocalUnsafe >= 8 ? L"yes" : L"no",
        m_lastMaxLocalUnsafe >= 18 ? L"yes" : L"no");''',
'''    const UINT cellCount = m_flowWidth * m_flowHeight;
    const double unsafeFraction = static_cast<double>(m_lastUnsafeCount) /
        std::max(1u, cellCount);
    const double badPercent = 100.0 * unsafeFraction;
    const double blendT = std::clamp((unsafeFraction - 0.25) / 0.15, 0.0, 1.0);
    const double safetyBlendPercent = 100.0 * blendT * blendT * (3.0 - 2.0 * blendT);
    return std::format(
        L"cut={}, bad {:.1f}% ({}/{}), safetyBlend {:.0f}%, worst7x7 {}/49, would8={}, would18={}",
        m_lastSceneCut ? L"yes" : L"no",
        badPercent, m_lastUnsafeCount, cellCount, safetyBlendPercent, m_lastMaxLocalUnsafe,
        m_lastMaxLocalUnsafe >= 8 ? L"yes" : L"no",
        m_lastMaxLocalUnsafe >= 18 ? L"yes" : L"no");''',
'telemetry text')

replace_once(
'''    const SeedParameters seedValues = {
        m_flowWidth, m_flowHeight, 4.0f, 20.0f,
        20.0f, {0.0f, 0.0f, 0.0f},
    };''',
'''    const SeedParameters seedValues = {
        m_flowWidth, m_flowHeight, 4.0f, 20.0f,
        20.0f, m_frameWidth, m_frameHeight, 0.86f,
        0.15f, 0.055f, {0.0f, 0.0f},
    };''',
'seed parameters')

replace_once(
'''    const std::array<ID3D11ShaderResourceView*, 2> seedInputs = {
        forwardFlowBtoA, backwardFlowAtoB,
    };''',
'''    const std::array<ID3D11ShaderResourceView*, 4> seedInputs = {
        forwardFlowBtoA, backwardFlowAtoB, previousFrame, nextFrame,
    };''',
'seed inputs')

replace_once(
'''                m_lastUnsafeCount = *static_cast<const UINT*>(qualityMapped.pData);
                m_lastMaxLocalUnsafe = *static_cast<const UINT*>(regionMapped.pData);''',
'''                const UINT packedQuality = *static_cast<const UINT*>(qualityMapped.pData);
                m_lastSceneCut = (packedQuality & 0x80000000u) != 0u;
                m_lastUnsafeCount = packedQuality & 0x7fffffffu;
                m_lastMaxLocalUnsafe = *static_cast<const UINT*>(regionMapped.pData);''',
'telemetry readback')

cpp_path.write_text(cpp, encoding='utf-8', newline='\n')

native_path = ROOT / 'Source/NvidiaOpticalFlowNative.cpp'
native = native_path.read_text(encoding='utf-8')
if 'frame quality gate' in native:
    native = native.replace('frame quality gate', 'scene-cut guard + adaptive high-motion safety blend')
    native_path.write_text(native, encoding='utf-8', newline='\n')

print('Patched scene-cut classifier and high-motion safety fallback.')
