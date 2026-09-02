from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Source"


def read(name: str) -> str:
    return (SOURCE / name).read_text(encoding="utf-8").replace("\r\n", "\n")


def write(name: str, text: str) -> None:
    (SOURCE / name).write_text(text.replace("\r\n", "\n"), encoding="utf-8", newline="\n")


def replace_exact(text: str, old: str, new: str, label: str) -> str:
    old = old.replace("\r\n", "\n")
    new = new.replace("\r\n", "\n")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def replace_regex(text: str, pattern: str, replacement: str, label: str) -> str:
    matches = list(re.finditer(pattern, text, flags=re.MULTILINE | re.DOTALL))
    if len(matches) != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {len(matches)}")
    return re.sub(pattern, lambda _: replacement, text, count=1, flags=re.MULTILINE | re.DOTALL)


# This patch intentionally runs *after* NvofAdaptiveSplatV12Patch.py. V1.2
# supplies the already-tested splat resources, dispatches and golden-anchored
# resolve. Build #3 replaces only its exploratory q proxy/global policy with
# the reconstructed modern 4x4 software-SAD confidence plus our independent
# dual-direction consensus authority.

# ---------------------------------------------------------------------------
# Seed: replace V1.2's single-sample photometric q proxy with the reconstructed
# modern 4x4 BT.709-limited luma SAD, exact pair-luma normalization, per-
# direction classifier counters, and the already-computed coarse source MAD.
# ---------------------------------------------------------------------------
seed = read("NvidiaOpticalFlowDenseSeed.hlsl")
seed = replace_exact(
    seed,
    """static const uint CatastrophicBit = 1u << 30u;\nstatic const uint FieldCountIncrement = 1u << 6u;\n""",
    """static const uint CatastrophicBit = 1u << 30u;\nstatic const uint StatsLocalWorst = 0u;\nstatic const uint StatsBaLow = 1u;\nstatic const uint StatsBaM1 = 2u;\nstatic const uint StatsBaM2 = 3u;\nstatic const uint StatsBaScene = 4u;\nstatic const uint StatsAbLow = 5u;\nstatic const uint StatsAbM1 = 6u;\nstatic const uint StatsAbM2 = 7u;\nstatic const uint StatsAbScene = 8u;\nstatic const uint StatsSourceMadMicro = 9u;\n""",
    "Seed Build3 stats constants",
)

seed = replace_regex(
    seed,
    r"^float Luma\(float3 rgb\)\n\{.*?^uint QuantizeError\(float errorPx\)\n\{.*?^\}\n",
    """uint Bt709LimitedY(float3 rgb)\n{\n    uint r = (uint)round(saturate(rgb.r) * 255.0);\n    uint g = (uint)round(saturate(rgb.g) * 255.0);\n    uint b = (uint)round(saturate(rgb.b) * 255.0);\n    float y = 16.0 + 0.182586 * float(r) + 0.614231 * float(g) + 0.062007 * float(b);\n    return (uint)clamp(round(y), 16.0, 235.0);\n}\n\nuint LoadY(Texture2D<float4> frame, int2 pixel)\n{\n    return Bt709LimitedY(LoadFrame(frame, pixel).rgb);\n}\n\nuint AbsDiffU(uint a, uint b)\n{\n    return a >= b ? a - b : b - a;\n}\n\nuint QuantizeQ(float q)\n{\n    return min((uint)round(min(max(q, 0.0), 8184.0) / 8.0), 1023u);\n}\n\nuint QuantizeError(float errorPx)\n{\n    return min((uint)ceil(max(errorPx, 0.0) / 2.0), 15u);\n}\n\nvoid AccumulateClassifierStats(uint q, uint lowIndex, uint m1Index, uint m2Index, uint sceneIndex)\n{\n    if (q < 200u) InterlockedAdd(PackedRegionStats[uint2(lowIndex, 0u)], 1u);\n    if (q >= 1600u) InterlockedAdd(PackedRegionStats[uint2(m1Index, 0u)], 1u);\n    if (q >= 2800u) InterlockedAdd(PackedRegionStats[uint2(m2Index, 0u)], 1u);\n    if (q >= 4000u) InterlockedAdd(PackedRegionStats[uint2(sceneIndex, 0u)], 1u);\n}\n""",
    "Seed modern-SAD helpers",
)

seed = replace_exact(
    seed,
    "bool DetectSceneCut()\n{\n",
    "bool DetectSceneCut(out float sourceMad)\n{\n",
    "Seed scene-cut signature",
)
seed = replace_exact(
    seed,
    """    float mad = float(sumAbs) / (n * 255.0);\n    float covariance = n * float(sumAB) - float(sumA) * float(sumB);\n""",
    """    float mad = float(sumAbs) / (n * 255.0);\n    sourceMad = mad;\n    float covariance = n * float(sumAB) - float(sumA) * float(sumB);\n""",
    "Seed expose source MAD",
)
seed = replace_exact(
    seed,
    """    if (all(id.xy == uint2(0, 0)) && DetectSceneCut()) {\n        InterlockedOr(UnsafeCellCount[uint2(0, 0)], SceneCutBit);\n    }\n\n    if (any(id.xy >= FlowSize)) return;\n""",
    """    if (all(id.xy == uint2(0, 0))) {\n        float sourceMad = 0.0;\n        if (DetectSceneCut(sourceMad)) {\n            InterlockedOr(UnsafeCellCount[uint2(0, 0)], SceneCutBit);\n        }\n        PackedRegionStats[uint2(StatsSourceMadMicro, 0u)] =\n            (uint)round(saturate(sourceMad) * 1000000.0);\n    }\n\n    if (any(id.xy >= FlowSize)) return;\n""",
    "Seed write source MAD",
)

seed = replace_regex(
    seed,
    r"^    // Clean-room q proxy:.*?^    bool forwardSeedValid = bToAError <= ConsistencyThreshold;",
    """    // Build #3 modern confidence. Reconstruct the proprietary normal-path\n    // score from the native NVOF vector itself: integer-displaced 4x4 source\n    // versus reference luma SAD. This is confidence evidence only; the frozen\n    // golden seed/salvage decision below is intentionally unchanged.\n    int2 displacementBtoA = int2(bToA); // float->int truncates toward zero\n    int2 displacementAtoB = int2(aToB);\n    int2 blockOrigin = int2(id.xy) * 4;\n    uint scoreBtoA = 0u;\n    uint scoreAtoB = 0u;\n    uint sourceLumaB = 0u;\n    uint sourceLumaA = 0u;\n\n    [unroll]\n    for (int by = 0; by < 4; ++by) {\n        [unroll]\n        for (int bx = 0; bx < 4; ++bx) {\n            int2 sourcePixel = blockOrigin + int2(bx, by);\n            uint yB = LoadY(NextFrame, sourcePixel);\n            uint yA = LoadY(PreviousFrame, sourcePixel);\n            uint yAReference = LoadY(PreviousFrame, sourcePixel + displacementBtoA);\n            uint yBReference = LoadY(NextFrame, sourcePixel + displacementAtoB);\n            sourceLumaB += yB;\n            sourceLumaA += yA;\n            scoreBtoA += AbsDiffU(yB, yAReference);\n            scoreAtoB += AbsDiffU(yA, yBReference);\n        }\n    }\n\n    uint lumaBtoA = sourceLumaB >> 4u;\n    uint lumaAtoB = sourceLumaA >> 4u;\n    float normalizedPairLuma = float(lumaBtoA + lumaAtoB) / 510.0;\n    uint pairLuma = (uint)(pow(normalizedPairLuma, 1.5) * 255.0);\n    if (pairLuma < 21u) pairLuma = 20u;\n    pairLuma &= 0xffu;\n\n    uint qBtoA = (scoreBtoA * 255u) / max(pairLuma, 1u);\n    uint qAtoB = (scoreAtoB * 255u) / max(pairLuma, 1u);\n\n    float2 metadataPreviousEndpoint = pixel + bToA;\n    float2 metadataNextEndpoint = pixel + aToB;\n    bool metadataBtoAInBounds = all(metadataPreviousEndpoint >= float2(0.0, 0.0))\n        && all(metadataPreviousEndpoint <= float2(FrameSize) - 1.0);\n    bool metadataAtoBInBounds = all(metadataNextEndpoint >= float2(0.0, 0.0))\n        && all(metadataNextEndpoint <= float2(FrameSize) - 1.0);\n\n    uint packedMetadata = QuantizeQ(float(qBtoA))\n        | (QuantizeQ(float(qAtoB)) << 10u)\n        | (QuantizeError(bToAError) << 20u)\n        | (QuantizeError(aToBError) << 24u)\n        | (metadataBtoAInBounds ? (1u << 28u) : 0u)\n        | (metadataAtoBInBounds ? (1u << 29u) : 0u)\n        | (catastrophic ? CatastrophicBit : 0u);\n    UnsafeCellMap[id.xy] = packedMetadata;\n\n    if (catastrophic) {\n        InterlockedAdd(UnsafeCellCount[uint2(0, 0)], 1u);\n    }\n\n    uint borderX = max(1u, (uint)(float(FlowSize.x) * 0.04));\n    uint borderY = max(1u, (uint)(float(FlowSize.y) * 0.04));\n    bool classifierInterior = id.x >= borderX && id.x < FlowSize.x - borderX\n        && id.y >= borderY && id.y < FlowSize.y - borderY;\n    if (classifierInterior) {\n        AccumulateClassifierStats(qBtoA, StatsBaLow, StatsBaM1, StatsBaM2, StatsBaScene);\n        AccumulateClassifierStats(qAtoB, StatsAbLow, StatsAbM1, StatsAbM2, StatsAbScene);\n    }\n\n    bool forwardSeedValid = bToAError <= ConsistencyThreshold;""",
    "Seed replace V1.2 q proxy",
)
write("NvidiaOpticalFlowDenseSeed.hlsl", seed)


# ---------------------------------------------------------------------------
# Region gate: slot zero remains the old worst-7x7 catastrophic telemetry.
# Slots 1..9 are owned by the Build #3 classifier/MAD statistics from Seed.
# ---------------------------------------------------------------------------
write(
    "NvidiaOpticalFlowDenseRegionGate.hlsl",
    """Texture2D<uint> PackedCellMetadata : register(t0);\nRWTexture2D<uint> PackedRegionStats : register(u0);\n\ncbuffer RegionGateParameters : register(b0)\n{\n    uint2 FlowSize;\n    uint MinUnsafeCells;\n    uint Radius;\n};\n\nstatic const uint CatastrophicBit = 1u << 30u;\nstatic const uint StatsLocalWorst = 0u;\n\nuint LoadUnsafe(int2 cell)\n{\n    if (any(cell < 0) || any(cell >= int2(FlowSize))) return 0u;\n    uint packed = PackedCellMetadata.Load(int3(cell, 0));\n    return (packed & CatastrophicBit) != 0u ? 1u : 0u;\n}\n\n[numthreads(8, 8, 1)]\nvoid main(uint3 id : SV_DispatchThreadID)\n{\n    if (any(id.xy >= FlowSize)) return;\n\n    int2 center = int2(id.xy);\n    if (LoadUnsafe(center) == 0u) return;\n\n    uint unsafeCount = 0u;\n    [loop]\n    for (int y = -int(Radius); y <= int(Radius); ++y) {\n        [loop]\n        for (int x = -int(Radius); x <= int(Radius); ++x) {\n            unsafeCount += LoadUnsafe(center + int2(x, y));\n        }\n    }\n    InterlockedMax(PackedRegionStats[uint2(StatsLocalWorst, 0u)], min(unsafeCount, 63u));\n}\n""",
)


# ---------------------------------------------------------------------------
# Shared classifier policy is duplicated in Scatter/Resolve so it remains GPU-
# resident and requires no synchronous CPU readback. Inclusive m1/m2/scene
# counters reproduce the exact class sums; zero skipping uses the proprietary
# two-thirds full-grid cap. Build #3 then adds our independent consensus/MAD
# authority ramp.
# ---------------------------------------------------------------------------
policy_helpers = """static const uint SceneCutBit = 0x80000000u;\nstatic const uint StatsBaLow = 1u;\nstatic const uint StatsBaM1 = 2u;\nstatic const uint StatsBaM2 = 3u;\nstatic const uint StatsBaScene = 4u;\nstatic const uint StatsAbLow = 5u;\nstatic const uint StatsAbM1 = 6u;\nstatic const uint StatsAbM2 = 7u;\nstatic const uint StatsAbScene = 8u;\nstatic const uint StatsSourceMadMicro = 9u;\n\nfloat SmoothstepScalar(float lo, float hi, float v)\n{\n    float x = saturate((v - lo) / max(hi - lo, 1.0e-6));\n    return x * x * (3.0 - 2.0 * x);\n}\n\nuint DirectionClass(uint lowCount, uint m1Count, uint m2Count, uint sceneCount,\n    uint interiorCount, uint fullCount)\n{\n    uint zeroSkipLimit = (2u * fullCount) / 3u;\n    uint zeroSkipped = min(lowCount, zeroSkipLimit);\n    uint considered = interiorCount > zeroSkipped ? interiorCount - zeroSkipped : 0u;\n    uint required = (20u * considered) / 100u;\n    if (sceneCount >= required) return 3u;\n    if (m2Count >= required) return 2u;\n    if (m1Count >= required) return 1u;\n    return 0u;\n}\n\nfloat FieldAuthority(Texture2D<uint> stats, Texture2D<uint> quality, uint2 flowSize)\n{\n    if ((quality.Load(int3(0, 0, 0)) & SceneCutBit) != 0u) return 0.0;\n\n    uint borderX = max(1u, (uint)(float(flowSize.x) * 0.04));\n    uint borderY = max(1u, (uint)(float(flowSize.y) * 0.04));\n    uint interiorW = flowSize.x > 2u * borderX ? flowSize.x - 2u * borderX : 1u;\n    uint interiorH = flowSize.y > 2u * borderY ? flowSize.y - 2u * borderY : 1u;\n    uint interiorCount = max(1u, interiorW * interiorH);\n    uint fullCount = max(1u, flowSize.x * flowSize.y);\n\n    uint baLow = stats.Load(int3(StatsBaLow, 0, 0));\n    uint baM1 = stats.Load(int3(StatsBaM1, 0, 0));\n    uint baM2 = stats.Load(int3(StatsBaM2, 0, 0));\n    uint baScene = stats.Load(int3(StatsBaScene, 0, 0));\n    uint abLow = stats.Load(int3(StatsAbLow, 0, 0));\n    uint abM1 = stats.Load(int3(StatsAbM1, 0, 0));\n    uint abM2 = stats.Load(int3(StatsAbM2, 0, 0));\n    uint abScene = stats.Load(int3(StatsAbScene, 0, 0));\n\n    uint baZeroSkipped = min(baLow, (2u * fullCount) / 3u);\n    uint abZeroSkipped = min(abLow, (2u * fullCount) / 3u);\n    uint baConsidered = max(1u, interiorCount > baZeroSkipped ? interiorCount - baZeroSkipped : 1u);\n    uint abConsidered = max(1u, interiorCount > abZeroSkipped ? interiorCount - abZeroSkipped : 1u);\n    uint baClass = DirectionClass(baLow, baM1, baM2, baScene, interiorCount, fullCount);\n    uint abClass = DirectionClass(abLow, abM1, abM2, abScene, interiorCount, fullCount);\n\n    bool ordinaryConsensus = baClass >= 1u && baClass <= 2u\n        && abClass >= 1u && abClass <= 2u;\n    if (!ordinaryConsensus) return 0.0;\n\n    float baM1Percent = 100.0 * float(baM1) / float(baConsidered);\n    float abM1Percent = 100.0 * float(abM1) / float(abConsidered);\n    float dualM1 = min(baM1Percent, abM1Percent);\n    float occupancyGain = SmoothstepScalar(20.0, 27.0, dualM1);\n    float sourceMad = float(stats.Load(int3(StatsSourceMadMicro, 0, 0))) / 1000000.0;\n    float madGuard = 1.0 - SmoothstepScalar(0.075, 0.095, sourceMad);\n    return occupancyGain * madGuard;\n}\n"""

write(
    "NvidiaOpticalFlowAdaptiveSplatScatter.hlsl",
    """Texture2D<int2> ForwardFlowBtoA : register(t0);\nTexture2D<int2> BackwardFlowAtoB : register(t1);\nTexture2D<uint> PackedCellMetadata : register(t2);\nTexture2D<uint> PackedQuality : register(t3);\nTexture2D<uint> PackedRegionStats : register(t4);\n\nRWStructuredBuffer<int> SplatAccum : register(u0);\n\ncbuffer SplatParameters : register(b0)\n{\n    uint2 FlowSize;\n    float GridSize;\n    float MidpointTime;\n};\n\nstatic const float WeightScale = 60.0;\n\n""" + policy_helpers + """\nfloat2 LoadFlow(Texture2D<int2> tex, uint2 cell)\n{\n    return float2(tex.Load(int3(cell, 0))) / 32.0;\n}\n\nvoid ScatterDirection(uint2 sourceCell, float2 flow, uint errorQ, uint qQ,\n    bool endpointInBounds, uint accumulatorOffset)\n{\n    if (!endpointInBounds || errorQ > 10u) return;\n\n    float errorPx = float(errorQ * 2u);\n    float q = float(qQ * 8u);\n    float confidence = exp(-min(errorPx, 40.0) / 8.0)\n        * exp(-min(q, 8000.0) / 1600.0);\n\n    static const float Phase = 0.5;\n    float2 targetCell = float2(sourceCell) + Phase * flow / GridSize;\n    int2 baseCell = int2(floor(targetCell));\n    float2 fracCell = frac(targetCell);\n    float2 inverseDisplacement = -Phase * flow;\n\n    [unroll]\n    for (uint oy = 0u; oy < 2u; ++oy) {\n        int targetY = baseCell.y + int(oy);\n        if (targetY < 0 || targetY >= int(FlowSize.y)) continue;\n        float wy = oy == 0u ? 1.0 - fracCell.y : fracCell.y;\n        [unroll]\n        for (uint ox = 0u; ox < 2u; ++ox) {\n            int targetX = baseCell.x + int(ox);\n            if (targetX < 0 || targetX >= int(FlowSize.x)) continue;\n            float wx = ox == 0u ? 1.0 - fracCell.x : fracCell.x;\n            int weightQ = (int)round(wx * wy * confidence * WeightScale);\n            if (weightQ <= 0) continue;\n            uint targetIndex = uint(targetY) * FlowSize.x + uint(targetX);\n            uint base = targetIndex * 6u + accumulatorOffset;\n            InterlockedAdd(SplatAccum[base + 0u], (int)round(inverseDisplacement.x * float(weightQ)));\n            InterlockedAdd(SplatAccum[base + 1u], (int)round(inverseDisplacement.y * float(weightQ)));\n            InterlockedAdd(SplatAccum[base + 2u], weightQ);\n        }\n    }\n}\n\n[numthreads(8, 8, 1)]\nvoid main(uint3 id : SV_DispatchThreadID)\n{\n    if (any(id.xy >= FlowSize)) return;\n    if (abs(MidpointTime - 0.5) > 1.0e-4) return;\n    if (FieldAuthority(PackedRegionStats, PackedQuality, FlowSize) <= 0.0) return;\n\n    uint packed = PackedCellMetadata.Load(int3(id.xy, 0));\n    uint qBtoA = packed & 0x3ffu;\n    uint qAtoB = (packed >> 10u) & 0x3ffu;\n    uint errBtoA = (packed >> 20u) & 0x0fu;\n    uint errAtoB = (packed >> 24u) & 0x0fu;\n    bool inBoundsBtoA = (packed & (1u << 28u)) != 0u;\n    bool inBoundsAtoB = (packed & (1u << 29u)) != 0u;\n\n    ScatterDirection(id.xy, LoadFlow(BackwardFlowAtoB, id.xy),\n        errAtoB, qAtoB, inBoundsAtoB, 0u);\n    ScatterDirection(id.xy, LoadFlow(ForwardFlowBtoA, id.xy),\n        errBtoA, qBtoA, inBoundsBtoA, 3u);\n}\n""",
)

write(
    "NvidiaOpticalFlowAdaptiveSplatResolve.hlsl",
    """StructuredBuffer<int> SplatAccum : register(t0);\nTexture2D<uint> PackedCellMetadata : register(t1);\nTexture2D<uint> PackedRegionStats : register(t2);\nTexture2D<uint> PackedQuality : register(t3);\n\nRWTexture2DArray<float4> ResolvedSplat : register(u0);\n\ncbuffer ResolveParameters : register(b0)\n{\n    uint2 FlowSize;\n    float WeightScale;\n    float Padding;\n};\n\n""" + policy_helpers + """\nfloat3 ResolveSide(uint cellIndex, uint offset)\n{\n    uint base = cellIndex * 6u + offset;\n    int sumX = SplatAccum[base + 0u];\n    int sumY = SplatAccum[base + 1u];\n    int sumW = SplatAccum[base + 2u];\n    if (sumW <= 0) return 0.0;\n    float invW = 1.0 / float(sumW);\n    return float3(float(sumX) * invW, float(sumY) * invW,\n        saturate(float(sumW) / max(WeightScale, 1.0)));\n}\n\n[numthreads(8, 8, 1)]\nvoid main(uint3 id : SV_DispatchThreadID)\n{\n    if (any(id.xy >= FlowSize)) return;\n    uint cellIndex = id.y * FlowSize.x + id.x;\n    float3 previous = ResolveSide(cellIndex, 0u);\n    float3 next = ResolveSide(cellIndex, 3u);\n\n    uint packed = PackedCellMetadata.Load(int3(id.xy, 0));\n    float qBtoA = float(packed & 0x3ffu) * 8.0;\n    float qAtoB = float((packed >> 10u) & 0x3ffu) * 8.0;\n    bool inBoundsBtoA = (packed & (1u << 28u)) != 0u;\n    bool inBoundsAtoB = (packed & (1u << 29u)) != 0u;\n    float combinedQ = 0.5 * ((inBoundsBtoA ? qBtoA : 0.0)\n        + (inBoundsAtoB ? qAtoB : 0.0));\n\n    ResolvedSplat[uint3(id.xy, 0u)] = float4(previous.xy, previous.z, combinedQ);\n    ResolvedSplat[uint3(id.xy, 1u)] = float4(next.xy, next.z,\n        FieldAuthority(PackedRegionStats, PackedQuality, FlowSize));\n}\n""",
)

# V1.2 Resolve did not bind the quality texture because its field authority had
# no independent cut input. Build #3 already passes m_qualityView to Scatter;
# add it to Resolve as a fourth SRV so FieldAuthority can preserve hard cuts.
cpp = read("NvidiaOpticalFlowDenseSynthesizer.cpp")
cpp = replace_exact(
    cpp,
    """    hr = device->CreateTexture2D(&qualityDesc, nullptr, &m_regionRejectTexture);\n""",
    """    D3D11_TEXTURE2D_DESC regionStatsDesc = qualityDesc;\n    regionStatsDesc.Width = 10;\n    hr = device->CreateTexture2D(&regionStatsDesc, nullptr, &m_regionRejectTexture);\n""",
    "C++ Build3 stats texture width",
)
cpp = replace_exact(
    cpp,
    """    D3D11_TEXTURE2D_DESC telemetryDesc = qualityDesc;\n    telemetryDesc.Usage = D3D11_USAGE_STAGING;\n    telemetryDesc.BindFlags = 0;\n    telemetryDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;\n""",
    """    D3D11_TEXTURE2D_DESC telemetryDesc = qualityDesc;\n    telemetryDesc.Usage = D3D11_USAGE_STAGING;\n    telemetryDesc.BindFlags = 0;\n    telemetryDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;\n    D3D11_TEXTURE2D_DESC regionTelemetryDesc = regionStatsDesc;\n    regionTelemetryDesc.Usage = D3D11_USAGE_STAGING;\n    regionTelemetryDesc.BindFlags = 0;\n    regionTelemetryDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;\n""",
    "C++ Build3 stats staging desc",
)
cpp = replace_exact(
    cpp,
    """        hr = device->CreateTexture2D(&telemetryDesc, nullptr, &m_regionReadback[slot]);\n""",
    """        hr = device->CreateTexture2D(&regionTelemetryDesc, nullptr, &m_regionReadback[slot]);\n""",
    "C++ Build3 stats staging texture",
)
cpp = replace_exact(
    cpp,
    """    const std::array<ID3D11ShaderResourceView*, 3> resolveInputs = {\n        m_splatAccumView, m_unsafeCellView, m_regionRejectView,\n    };\n""",
    """    const std::array<ID3D11ShaderResourceView*, 4> resolveInputs = {\n        m_splatAccumView, m_unsafeCellView, m_regionRejectView, m_qualityView,\n    };\n""",
    "C++ Build3 Resolve quality input",
)
cpp = replace_exact(
    cpp,
    """                const UINT packedRegion = *static_cast<const UINT*>(regionMapped.pData);\n                m_lastMaxLocalUnsafe = packedRegion & 0x3fu;\n                m_lastFieldBadCount = packedRegion >> 6u;\n                m_haveTelemetry = true;\n""",
    """                const UINT* stats = static_cast<const UINT*>(regionMapped.pData);\n                m_lastMaxLocalUnsafe = stats[0];\n                m_lastBaLowCount = stats[1];\n                m_lastBaM1Count = stats[2];\n                m_lastBaM2Count = stats[3];\n                m_lastBaSceneCount = stats[4];\n                m_lastAbLowCount = stats[5];\n                m_lastAbM1Count = stats[6];\n                m_lastAbM2Count = stats[7];\n                m_lastAbSceneCount = stats[8];\n                m_lastSourceMadMicro = stats[9];\n                m_haveTelemetry = true;\n""",
    "C++ Build3 telemetry decode",
)

cpp = replace_regex(
    cpp,
    r"^std::wstring CNvidiaOpticalFlowDenseSynthesizer::GetTelemetryText\(\) const\n\{.*?^\}\n",
    """std::wstring CNvidiaOpticalFlowDenseSynthesizer::GetTelemetryText() const\n{\n    if (!m_haveTelemetry || !m_flowWidth || !m_flowHeight) {\n        return L\"quality telemetry warming up\";\n    }\n\n    const UINT cellCount = m_flowWidth * m_flowHeight;\n    const double badPercent = 100.0 * static_cast<double>(m_lastUnsafeCount) / std::max(1u, cellCount);\n    const UINT borderX = std::max(1u, static_cast<UINT>(static_cast<double>(m_flowWidth) * 0.04));\n    const UINT borderY = std::max(1u, static_cast<UINT>(static_cast<double>(m_flowHeight) * 0.04));\n    const UINT interiorWidth = m_flowWidth > 2u * borderX ? m_flowWidth - 2u * borderX : 1u;\n    const UINT interiorHeight = m_flowHeight > 2u * borderY ? m_flowHeight - 2u * borderY : 1u;\n    const UINT interiorCount = std::max(1u, interiorWidth * interiorHeight);\n    const UINT zeroSkipLimit = (2u * cellCount) / 3u;\n\n    const auto classify = [interiorCount, zeroSkipLimit](const UINT low, const UINT m1,\n        const UINT m2, const UINT scene) -> std::pair<UINT, UINT> {\n        const UINT zeroSkipped = std::min(low, zeroSkipLimit);\n        const UINT considered = std::max(1u, interiorCount > zeroSkipped ? interiorCount - zeroSkipped : 1u);\n        const UINT required = (20u * considered) / 100u;\n        if (scene >= required) return {3u, considered};\n        if (m2 >= required) return {2u, considered};\n        if (m1 >= required) return {1u, considered};\n        return {0u, considered};\n    };\n    const auto ba = classify(m_lastBaLowCount, m_lastBaM1Count, m_lastBaM2Count, m_lastBaSceneCount);\n    const auto ab = classify(m_lastAbLowCount, m_lastAbM1Count, m_lastAbM2Count, m_lastAbSceneCount);\n    const double baM1 = 100.0 * static_cast<double>(m_lastBaM1Count) / ba.second;\n    const double abM1 = 100.0 * static_cast<double>(m_lastAbM1Count) / ab.second;\n    const double dualM1 = std::min(baM1, abM1);\n    const double sourceMad = static_cast<double>(m_lastSourceMadMicro) / 1000000.0;\n    const auto smoothstep = [](const double lo, const double hi, const double value) {\n        const double x = std::clamp((value - lo) / (hi - lo), 0.0, 1.0);\n        return x * x * (3.0 - 2.0 * x);\n    };\n    const bool consensus = ba.first >= 1u && ba.first <= 2u && ab.first >= 1u && ab.first <= 2u;\n    const double occupancyGain = smoothstep(20.0, 27.0, dualM1);\n    const double madGuard = 1.0 - smoothstep(0.075, 0.095, sourceMad);\n    const double gain = (!m_lastSceneCut && consensus) ? occupancyGain * madGuard : 0.0;\n\n    return std::format(\n        L\"cut={}, cls BA/AB={}/{}, m1 BA/AB={:.1f}/{:.1f}%, dual={:.1f}%, mad={:.3f}, gain={:.3f}, bad={:.1f}%, worst7x7={}/49, guard=modern-sad-consensus-build3\",\n        m_lastSceneCut ? L\"yes\" : L\"no\", ba.first, ab.first, baM1, abM1,\n        dualM1, sourceMad, gain, badPercent, m_lastMaxLocalUnsafe);\n}\n""",
    "C++ Build3 telemetry text",
)
write("NvidiaOpticalFlowDenseSynthesizer.cpp", cpp)

header = read("NvidiaOpticalFlowDenseSynthesizer.h")
header = replace_exact(
    header,
    """    UINT m_lastMaxLocalUnsafe = 0;\n    UINT m_lastFieldBadCount = 0;\n    bool m_haveTelemetry = false;\n""",
    """    UINT m_lastMaxLocalUnsafe = 0;\n    UINT m_lastBaLowCount = 0;\n    UINT m_lastBaM1Count = 0;\n    UINT m_lastBaM2Count = 0;\n    UINT m_lastBaSceneCount = 0;\n    UINT m_lastAbLowCount = 0;\n    UINT m_lastAbM1Count = 0;\n    UINT m_lastAbM2Count = 0;\n    UINT m_lastAbSceneCount = 0;\n    UINT m_lastSourceMadMicro = 0;\n    bool m_haveTelemetry = false;\n""",
    "Header Build3 telemetry fields",
)
write("NvidiaOpticalFlowDenseSynthesizer.h", header)

# Reset block still names the V1.2 single counter; replace it after the header
# transformation so all Build #3 telemetry is reset deterministically.
cpp = read("NvidiaOpticalFlowDenseSynthesizer.cpp")
cpp = replace_exact(
    cpp,
    """    m_lastMaxLocalUnsafe = 0;\n    m_lastFieldBadCount = 0;\n    m_haveTelemetry = false;\n""",
    """    m_lastMaxLocalUnsafe = 0;\n    m_lastBaLowCount = 0;\n    m_lastBaM1Count = 0;\n    m_lastBaM2Count = 0;\n    m_lastBaSceneCount = 0;\n    m_lastAbLowCount = 0;\n    m_lastAbM1Count = 0;\n    m_lastAbM2Count = 0;\n    m_lastAbSceneCount = 0;\n    m_lastSourceMadMicro = 0;\n    m_haveTelemetry = false;\n""",
    "C++ Build3 reset telemetry",
)
write("NvidiaOpticalFlowDenseSynthesizer.cpp", cpp)

warp = read("NvidiaOpticalFlowDenseWarp.hlsl")
warp = replace_exact(
    warp,
    "float qAuthority = smoothstep(1200.0, 2400.0, previousSplat.w);\n",
    "float qAuthority = smoothstep(1000.0, 2200.0, previousSplat.w);\n",
    "Warp Build3 local q authority",
)
write("NvidiaOpticalFlowDenseWarp.hlsl", warp)

print("Applied NVOF Build #3 modern-SAD dual-direction consensus patch.")
