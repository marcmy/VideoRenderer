from __future__ import annotations

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


# ---------------------------------------------------------------------------
# Seed: retain the frozen golden validation/salvage path, but pack the coarse
# metadata needed by V1.2 and count q>=1600 cells in the 4%-trimmed interior.
# The temporal-motion salvage patch is applied before this script in CI.
# ---------------------------------------------------------------------------
seed = read("NvidiaOpticalFlowDenseSeed.hlsl")
seed = replace_exact(
    seed,
    """Texture2D<float4> PreviousFrame : register(t2);\nTexture2D<float4> NextFrame : register(t3);\nRWTexture2D<uint> SeedMap : register(u0);\nRWTexture2D<uint> UnsafeCellCount : register(u1);\nRWTexture2D<uint> UnsafeCellMap : register(u2);\nRWTexture2D<float4> RepairCandidate : register(u3);\n""",
    """Texture2D<float4> PreviousFrame : register(t2);\nTexture2D<float4> NextFrame : register(t3);\nSamplerState LinearClamp : register(s0);\nRWTexture2D<uint> SeedMap : register(u0);\nRWTexture2D<uint> UnsafeCellCount : register(u1);\nRWTexture2D<uint> UnsafeCellMap : register(u2);\nRWTexture2D<float4> RepairCandidate : register(u3);\nRWTexture2D<uint> PackedRegionStats : register(u4);\n""",
    "Seed bindings",
)
seed = replace_exact(
    seed,
    """static const uint CutSampleWidth = 32u;\nstatic const uint CutSampleHeight = 18u;\nstatic const uint CutSampleCount = CutSampleWidth * CutSampleHeight;\n""",
    """static const uint CutSampleWidth = 32u;\nstatic const uint CutSampleHeight = 18u;\nstatic const uint CutSampleCount = CutSampleWidth * CutSampleHeight;\nstatic const uint CatastrophicBit = 1u << 30u;\nstatic const uint FieldCountIncrement = 1u << 6u;\n""",
    "Seed metadata constants",
)
seed = replace_exact(
    seed,
    """float4 SampleFrame(Texture2D<float4> frame, float2 pixel)\n{\n    pixel = clamp(pixel, float2(0.0, 0.0), float2(FrameSize) - 1.0);\n    int2 base = int2(floor(pixel));\n    float2 f = frac(pixel);\n\n    float4 v00 = LoadFrame(frame, base);\n    float4 v10 = LoadFrame(frame, base + int2(1, 0));\n    float4 v01 = LoadFrame(frame, base + int2(0, 1));\n    float4 v11 = LoadFrame(frame, base + int2(1, 1));\n    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);\n}\n""",
    """float4 SampleFrame(Texture2D<float4> frame, float2 pixel)\n{\n    pixel = clamp(pixel, float2(0.0, 0.0), float2(FrameSize) - 1.0);\n    int2 base = int2(floor(pixel));\n    float2 f = frac(pixel);\n\n    float4 v00 = LoadFrame(frame, base);\n    float4 v10 = LoadFrame(frame, base + int2(1, 0));\n    float4 v01 = LoadFrame(frame, base + int2(0, 1));\n    float4 v11 = LoadFrame(frame, base + int2(1, 1));\n    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);\n}\n\nfloat Luma(float3 rgb)\n{\n    return dot(rgb, float3(0.2126, 0.7152, 0.0722));\n}\n\nfloat4 SampleFrameLinear(Texture2D<float4> frame, float2 pixel)\n{\n    pixel = clamp(pixel, float2(0.0, 0.0), float2(FrameSize) - 1.0);\n    float2 uv = (pixel + 0.5) / float2(FrameSize);\n    return frame.SampleLevel(LinearClamp, uv, 0.0);\n}\n\nuint QuantizeQ(float q)\n{\n    return min((uint)round(min(max(q, 0.0), 8184.0) / 8.0), 1023u);\n}\n\nuint QuantizeError(float errorPx)\n{\n    return min((uint)ceil(max(errorPx, 0.0) / 2.0), 15u);\n}\n""",
    "Seed q helpers",
)
seed = replace_exact(
    seed,
    """    bool catastrophic = motion > MotionThreshold && consistency > ConsistencyThreshold;\n    UnsafeCellMap[id.xy] = catastrophic ? 1u : 0u;\n    if (catastrophic) {\n        InterlockedAdd(UnsafeCellCount[uint2(0, 0)], 1u);\n    }\n""",
    """    bool catastrophic = motion > MotionThreshold && consistency > ConsistencyThreshold;\n\n    // Clean-room q proxy: one representative 4x4-cell sample in each native\n    // direction. This is classifier/authority evidence only; it never changes\n    // the golden seed-validity decision below.\n    float2 metadataPreviousEndpoint = pixel + bToA;\n    float2 metadataNextEndpoint = pixel + aToB;\n    bool metadataBtoAInBounds = all(metadataPreviousEndpoint >= float2(0.0, 0.0))\n        && all(metadataPreviousEndpoint <= float2(FrameSize) - 1.0);\n    bool metadataAtoBInBounds = all(metadataNextEndpoint >= float2(0.0, 0.0))\n        && all(metadataNextEndpoint <= float2(FrameSize) - 1.0);\n\n    float yNext = Luma(LoadFrame(NextFrame, int2(pixel)).rgb);\n    float yPrevious = Luma(LoadFrame(PreviousFrame, int2(pixel)).rgb);\n    float qBtoA = 0.0;\n    float qAtoB = 0.0;\n    if (metadataBtoAInBounds) {\n        qBtoA = 16.0 * 255.0\n            * abs(yNext - Luma(SampleFrameLinear(PreviousFrame, metadataPreviousEndpoint).rgb))\n            / max(yNext, 1.0 / 255.0);\n    }\n    if (metadataAtoBInBounds) {\n        qAtoB = 16.0 * 255.0\n            * abs(yPrevious - Luma(SampleFrameLinear(NextFrame, metadataNextEndpoint).rgb))\n            / max(yPrevious, 1.0 / 255.0);\n    }\n    float qCombined = 0.5 * (qBtoA + qAtoB);\n\n    uint packedMetadata = QuantizeQ(qBtoA)\n        | (QuantizeQ(qAtoB) << 10u)\n        | (QuantizeError(bToAError) << 20u)\n        | (QuantizeError(aToBError) << 24u)\n        | (metadataBtoAInBounds ? (1u << 28u) : 0u)\n        | (metadataAtoBInBounds ? (1u << 29u) : 0u)\n        | (catastrophic ? CatastrophicBit : 0u);\n    UnsafeCellMap[id.xy] = packedMetadata;\n\n    if (catastrophic) {\n        InterlockedAdd(UnsafeCellCount[uint2(0, 0)], 1u);\n    }\n\n    uint borderX = max(1u, (uint)(float(FlowSize.x) * 0.04));\n    uint borderY = max(1u, (uint)(float(FlowSize.y) * 0.04));\n    bool classifierInterior = id.x >= borderX && id.x < FlowSize.x - borderX\n        && id.y >= borderY && id.y < FlowSize.y - borderY;\n    if (classifierInterior && qCombined >= 1600.0) {\n        InterlockedAdd(PackedRegionStats[uint2(0, 0)], FieldCountIncrement);\n    }\n""",
    "Seed packed metadata",
)
write("NvidiaOpticalFlowDenseSeed.hlsl", seed)


# ---------------------------------------------------------------------------
# RegionGate: bit 30 is the catastrophic flag. The low six bits of the 1x1
# region texture remain worst-7x7 telemetry; upper bits carry the q>=1600 field
# count written by Seed. Seed and RegionGate are separate dispatches, so the
# upper bits are stable while the low-bit InterlockedMax runs.
# ---------------------------------------------------------------------------
write(
    "NvidiaOpticalFlowDenseRegionGate.hlsl",
    """Texture2D<uint> PackedCellMetadata : register(t0);\nRWTexture2D<uint> PackedRegionStats : register(u0);\n\ncbuffer RegionGateParameters : register(b0)\n{\n    uint2 FlowSize;\n    uint MinUnsafeCells;\n    uint Radius;\n};\n\nstatic const uint CatastrophicBit = 1u << 30u;\nstatic const uint LowTelemetryMask = 63u;\n\nuint LoadUnsafe(int2 cell)\n{\n    if (any(cell < 0) || any(cell >= int2(FlowSize))) return 0u;\n    uint packed = PackedCellMetadata.Load(int3(cell, 0));\n    return (packed & CatastrophicBit) != 0u ? 1u : 0u;\n}\n\n[numthreads(8, 8, 1)]\nvoid main(uint3 id : SV_DispatchThreadID)\n{\n    if (any(id.xy >= FlowSize)) return;\n\n    int2 center = int2(id.xy);\n    if (LoadUnsafe(center) == 0u) return;\n\n    uint unsafeCount = 0u;\n    [loop]\n    for (int y = -int(Radius); y <= int(Radius); ++y) {\n        [loop]\n        for (int x = -int(Radius); x <= int(Radius); ++x) {\n            unsafeCount += LoadUnsafe(center + int2(x, y));\n        }\n    }\n\n    uint packedNow = PackedRegionStats[uint2(0, 0)];\n    uint fieldBits = packedNow & ~LowTelemetryMask;\n    uint candidate = fieldBits | min(unsafeCount, LowTelemetryMask);\n    InterlockedMax(PackedRegionStats[uint2(0, 0)], candidate);\n}\n""",
)


# ---------------------------------------------------------------------------
# Scatter: midpoint-only by design. 32-bit signed fixed-point overflow was
# proven for weight-scale 60 at t=.5; arbitrary phases intentionally take the
# untouched golden path until a separate accumulator-range design is proven.
# ---------------------------------------------------------------------------
write(
    "NvidiaOpticalFlowAdaptiveSplatScatter.hlsl",
    """Texture2D<int2> ForwardFlowBtoA : register(t0);\nTexture2D<int2> BackwardFlowAtoB : register(t1);\nTexture2D<uint> PackedCellMetadata : register(t2);\nTexture2D<uint> PackedQuality : register(t3);\nTexture2D<uint> PackedRegionStats : register(t4);\n\nRWStructuredBuffer<int> SplatAccum : register(u0);\n\ncbuffer SplatParameters : register(b0)\n{\n    uint2 FlowSize;\n    float GridSize;\n    float MidpointTime;\n};\n\nstatic const uint SceneCutBit = 0x80000000u;\nstatic const uint FieldCountShift = 6u;\nstatic const float WeightScale = 60.0;\n\nfloat2 LoadFlow(Texture2D<int2> tex, uint2 cell)\n{\n    return float2(tex.Load(int3(cell, 0))) / 32.0;\n}\n\nfloat SmoothstepScalar(float lo, float hi, float v)\n{\n    float x = saturate((v - lo) / max(hi - lo, 1.0e-6));\n    return x * x * (3.0 - 2.0 * x);\n}\n\nfloat FieldAuthority()\n{\n    if ((PackedQuality.Load(int3(0, 0, 0)) & SceneCutBit) != 0u) return 0.0;\n\n    uint borderX = max(1u, (uint)(float(FlowSize.x) * 0.04));\n    uint borderY = max(1u, (uint)(float(FlowSize.y) * 0.04));\n    uint interiorW = FlowSize.x > 2u * borderX ? FlowSize.x - 2u * borderX : 1u;\n    uint interiorH = FlowSize.y > 2u * borderY ? FlowSize.y - 2u * borderY : 1u;\n    uint interiorCount = max(1u, interiorW * interiorH);\n    uint badCount = PackedRegionStats.Load(int3(0, 0, 0)) >> FieldCountShift;\n    float badPercent = 100.0 * float(badCount) / float(interiorCount);\n    return SmoothstepScalar(15.0, 25.0, badPercent);\n}\n\nvoid ScatterDirection(uint2 sourceCell, float2 flow, uint errorQ, uint qQ,\n    bool endpointInBounds, uint accumulatorOffset)\n{\n    if (!endpointInBounds || errorQ > 10u) return;\n\n    float errorPx = float(errorQ * 2u);\n    float q = float(qQ * 8u);\n    float confidence = exp(-min(errorPx, 40.0) / 8.0)\n        * exp(-min(q, 8000.0) / 1200.0);\n\n    static const float Phase = 0.5;\n    float2 targetCell = float2(sourceCell) + Phase * flow / GridSize;\n    int2 baseCell = int2(floor(targetCell));\n    float2 fracCell = frac(targetCell);\n    float2 inverseDisplacement = -Phase * flow;\n\n    [unroll]\n    for (uint oy = 0u; oy < 2u; ++oy) {\n        int targetY = baseCell.y + int(oy);\n        if (targetY < 0 || targetY >= int(FlowSize.y)) continue;\n        float wy = oy == 0u ? 1.0 - fracCell.y : fracCell.y;\n\n        [unroll]\n        for (uint ox = 0u; ox < 2u; ++ox) {\n            int targetX = baseCell.x + int(ox);\n            if (targetX < 0 || targetX >= int(FlowSize.x)) continue;\n            float wx = ox == 0u ? 1.0 - fracCell.x : fracCell.x;\n            int weightQ = (int)round(wx * wy * confidence * WeightScale);\n            if (weightQ <= 0) continue;\n\n            uint targetIndex = uint(targetY) * FlowSize.x + uint(targetX);\n            uint base = targetIndex * 6u + accumulatorOffset;\n            int weightedX = (int)round(inverseDisplacement.x * float(weightQ));\n            int weightedY = (int)round(inverseDisplacement.y * float(weightQ));\n            InterlockedAdd(SplatAccum[base + 0u], weightedX);\n            InterlockedAdd(SplatAccum[base + 1u], weightedY);\n            InterlockedAdd(SplatAccum[base + 2u], weightQ);\n        }\n    }\n}\n\n[numthreads(8, 8, 1)]\nvoid main(uint3 id : SV_DispatchThreadID)\n{\n    if (any(id.xy >= FlowSize)) return;\n    if (abs(MidpointTime - 0.5) > 1.0e-4) return;\n    if (FieldAuthority() <= 0.0) return;\n\n    uint packed = PackedCellMetadata.Load(int3(id.xy, 0));\n    uint qBtoA = packed & 0x3ffu;\n    uint qAtoB = (packed >> 10u) & 0x3ffu;\n    uint errBtoA = (packed >> 20u) & 0x0fu;\n    uint errAtoB = (packed >> 24u) & 0x0fu;\n    bool inBoundsBtoA = (packed & (1u << 28u)) != 0u;\n    bool inBoundsAtoB = (packed & (1u << 29u)) != 0u;\n\n    // A-side map: A->B vector projected to midpoint, then inverted back to A.\n    ScatterDirection(id.xy, LoadFlow(BackwardFlowAtoB, id.xy),\n        errAtoB, qAtoB, inBoundsAtoB, 0u);\n    // B-side map: B->A vector projected to midpoint, then inverted back to B.\n    ScatterDirection(id.xy, LoadFlow(ForwardFlowBtoA, id.xy),\n        errBtoA, qBtoA, inBoundsBtoA, 3u);\n}\n""",
)


# ---------------------------------------------------------------------------
# Resolve: normalize the six-int accumulator into a two-slice FP16 map.
# slice 0 = previous inverse displacement/support/combined q
# slice 1 = next inverse displacement/support/global field authority
# ---------------------------------------------------------------------------
write(
    "NvidiaOpticalFlowAdaptiveSplatResolve.hlsl",
    """StructuredBuffer<int> SplatAccum : register(t0);\nTexture2D<uint> PackedCellMetadata : register(t1);\nTexture2D<uint> PackedRegionStats : register(t2);\n\nRWTexture2DArray<float4> ResolvedSplat : register(u0);\n\ncbuffer ResolveParameters : register(b0)\n{\n    uint2 FlowSize;\n    float WeightScale;\n    float Padding;\n};\n\nfloat SmoothstepScalar(float lo, float hi, float v)\n{\n    float x = saturate((v - lo) / max(hi - lo, 1.0e-6));\n    return x * x * (3.0 - 2.0 * x);\n}\n\nfloat3 ResolveSide(uint cellIndex, uint offset)\n{\n    uint base = cellIndex * 6u + offset;\n    int sumX = SplatAccum[base + 0u];\n    int sumY = SplatAccum[base + 1u];\n    int sumW = SplatAccum[base + 2u];\n    if (sumW <= 0) return 0.0;\n    float invW = 1.0 / float(sumW);\n    return float3(float(sumX) * invW, float(sumY) * invW,\n        saturate(float(sumW) / max(WeightScale, 1.0)));\n}\n\nfloat FieldAuthority()\n{\n    uint borderX = max(1u, (uint)(float(FlowSize.x) * 0.04));\n    uint borderY = max(1u, (uint)(float(FlowSize.y) * 0.04));\n    uint interiorW = FlowSize.x > 2u * borderX ? FlowSize.x - 2u * borderX : 1u;\n    uint interiorH = FlowSize.y > 2u * borderY ? FlowSize.y - 2u * borderY : 1u;\n    uint interiorCount = max(1u, interiorW * interiorH);\n    uint badCount = PackedRegionStats.Load(int3(0, 0, 0)) >> 6u;\n    return SmoothstepScalar(15.0, 25.0,\n        100.0 * float(badCount) / float(interiorCount));\n}\n\n[numthreads(8, 8, 1)]\nvoid main(uint3 id : SV_DispatchThreadID)\n{\n    if (any(id.xy >= FlowSize)) return;\n\n    uint cellIndex = id.y * FlowSize.x + id.x;\n    float3 previous = ResolveSide(cellIndex, 0u);\n    float3 next = ResolveSide(cellIndex, 3u);\n\n    uint packed = PackedCellMetadata.Load(int3(id.xy, 0));\n    float qBtoA = float(packed & 0x3ffu) * 8.0;\n    float qAtoB = float((packed >> 10u) & 0x3ffu) * 8.0;\n    bool inBoundsBtoA = (packed & (1u << 28u)) != 0u;\n    bool inBoundsAtoB = (packed & (1u << 29u)) != 0u;\n    float combinedQ = 0.5 * (\n        (inBoundsBtoA ? qBtoA : 0.0)\n        + (inBoundsAtoB ? qAtoB : 0.0));\n\n    ResolvedSplat[uint3(id.xy, 0u)] =\n        float4(previous.xy, previous.z, combinedQ);\n    ResolvedSplat[uint3(id.xy, 1u)] =\n        float4(next.xy, next.z, FieldAuthority());\n}\n""",
)


# ---------------------------------------------------------------------------
# Warp: preserve the golden shader verbatim through its local temporal fallback,
# then add a bounded V1.2 post-golden robust vote at exact midpoint only.
# ---------------------------------------------------------------------------
warp = read("NvidiaOpticalFlowDenseWarp.hlsl")
warp = replace_exact(
    warp,
    """Texture2D<uint> UnsafeCellCount : register(t3);\nTexture2D<float4> RepairField : register(t4);\n""",
    """Texture2D<uint> UnsafeCellCount : register(t3);\nTexture2D<float4> RepairField : register(t4);\nTexture2DArray<float4> ResolvedSplat : register(t5);\n""",
    "Warp adaptive SRV",
)
warp = replace_exact(
    warp,
    """cbuffer WarpParameters : register(b0)\n{\n    uint2 FrameSize;\n    uint FlowCellCount;\n    float RepeatBadFraction;\n    float MidpointTime;\n    float RepairGridSize;\n    float2 Padding;\n};\n""",
    """cbuffer WarpParameters : register(b0)\n{\n    uint2 FrameSize;\n    uint2 FlowSize;\n    float MidpointTime;\n    float RepairGridSize;\n    float2 Padding;\n};\n""",
    "Warp parameters",
)
warp = replace_exact(
    warp,
    """float4 SampleFrame(Texture2D<float4> frame, float2 pixel)\n{\n    return frame.SampleLevel(LinearClamp, PixelToUv(pixel), 0.0);\n}\n""",
    """float4 SampleFrame(Texture2D<float4> frame, float2 pixel)\n{\n    return frame.SampleLevel(LinearClamp, PixelToUv(pixel), 0.0);\n}\n\nfloat4 SampleSplat(float2 pixel, uint slice)\n{\n    float2 coarse = pixel / max(RepairGridSize, 1.0e-6);\n    float2 uv = (coarse + 0.5) / float2(FlowSize);\n    return ResolvedSplat.SampleLevel(LinearClamp, float3(uv, float(slice)), 0.0);\n}\n\nfloat4 Median3(float4 a, float4 b, float4 c)\n{\n    return a + b + c - min(a, min(b, c)) - max(a, max(b, c));\n}\n""",
    "Warp splat helpers",
)
warp = replace_exact(
    warp,
    """    OutputFrame[id.xy] = current;\n}\n""",
    """    // V1.2 adaptive-splat vote. The frozen golden path above remains the\n    // primary hypothesis. Only the currently proven exact-midpoint scheduler\n    // enables this path; arbitrary phases intentionally remain golden-only.\n    if (abs(MidpointTime - 0.5) <= 1.0e-4) {\n        float4 previousSplat = SampleSplat(target, 0u);\n        float4 nextSplat = SampleSplat(target, 1u);\n        float maxSupport = max(previousSplat.z, nextSplat.z);\n        float fieldAuthority = saturate(nextSplat.w);\n\n        if (fieldAuthority > 0.0 && maxSupport > 1.0e-6) {\n            float4 temporalMidpoint = lerp(\n                SampleFrame(PreviousFrame, target),\n                SampleFrame(NextFrame, target),\n                MidpointTime);\n            float4 splatPrevious = SampleFrame(\n                PreviousFrame, target + previousSplat.xy);\n            float4 splatNext = SampleFrame(\n                NextFrame, target + nextSplat.xy);\n\n            float previousWeight = (1.0 - MidpointTime)\n                * pow(max(previousSplat.z, 1.0e-8), 3.0);\n            float nextWeight = MidpointTime\n                * pow(max(nextSplat.z, 1.0e-8), 3.0);\n            float totalWeight = previousWeight + nextWeight;\n            float4 alternate = totalWeight > 1.0e-7\n                ? (splatPrevious * previousWeight + splatNext * nextWeight) / totalWeight\n                : temporalMidpoint;\n\n            float4 robust = Median3(current, alternate, temporalMidpoint);\n            float qAuthority = smoothstep(1200.0, 2400.0, previousSplat.w);\n            float supportAuthority = smoothstep(0.03, 0.22, maxSupport);\n            float localAuthority = min(qAuthority * supportAuthority, 0.60);\n            float alpha = localAuthority * fieldAuthority;\n            current = lerp(current, robust, alpha);\n        }\n    }\n\n    OutputFrame[id.xy] = current;\n}\n""",
    "Warp V1.2 vote",
)
write("NvidiaOpticalFlowDenseWarp.hlsl", warp)


# ---------------------------------------------------------------------------
# Header plumbing.
# ---------------------------------------------------------------------------
header = read("NvidiaOpticalFlowDenseSynthesizer.h")
header = replace_exact(
    header,
    """    struct WarpParameters {\n        UINT frameWidth;\n        UINT frameHeight;\n        UINT flowCellCount;\n        float repeatBadFraction;\n        float midpointTime;\n        float repairGridSize;\n        float padding[2];\n    };\n    static_assert(sizeof(WarpParameters) == 32);\n""",
    """    struct SplatParameters {\n        UINT flowWidth;\n        UINT flowHeight;\n        float gridSize;\n        float midpointTime;\n    };\n    static_assert(sizeof(SplatParameters) == 16);\n\n    struct ResolveParameters {\n        UINT flowWidth;\n        UINT flowHeight;\n        float weightScale;\n        float padding;\n    };\n    static_assert(sizeof(ResolveParameters) == 16);\n\n    struct WarpParameters {\n        UINT frameWidth;\n        UINT frameHeight;\n        UINT flowWidth;\n        UINT flowHeight;\n        float midpointTime;\n        float repairGridSize;\n        float padding[2];\n    };\n    static_assert(sizeof(WarpParameters) == 32);\n""",
    "Header adaptive params",
)
header = replace_exact(
    header,
    """    CComPtr<ID3D11ComputeShader> m_denseShader;\n    CComPtr<ID3D11ComputeShader> m_warpShader;\n""",
    """    CComPtr<ID3D11ComputeShader> m_denseShader;\n    CComPtr<ID3D11ComputeShader> m_splatShader;\n    CComPtr<ID3D11ComputeShader> m_resolveShader;\n    CComPtr<ID3D11ComputeShader> m_warpShader;\n""",
    "Header adaptive shaders",
)
header = replace_exact(
    header,
    """    CComPtr<ID3D11Texture2D> m_regionRejectTexture;\n    CComPtr<ID3D11ShaderResourceView> m_regionRejectView;\n    CComPtr<ID3D11UnorderedAccessView> m_regionRejectUav;\n""",
    """    CComPtr<ID3D11Texture2D> m_regionRejectTexture;\n    CComPtr<ID3D11ShaderResourceView> m_regionRejectView;\n    CComPtr<ID3D11UnorderedAccessView> m_regionRejectUav;\n\n    CComPtr<ID3D11Buffer> m_splatAccumBuffer;\n    CComPtr<ID3D11ShaderResourceView> m_splatAccumView;\n    CComPtr<ID3D11UnorderedAccessView> m_splatAccumUav;\n\n    CComPtr<ID3D11Texture2D> m_resolvedSplatTexture;\n    CComPtr<ID3D11ShaderResourceView> m_resolvedSplatView;\n    CComPtr<ID3D11UnorderedAccessView> m_resolvedSplatUav;\n""",
    "Header adaptive resources",
)
header = replace_exact(
    header,
    """    UINT m_lastUnsafeCount = 0;\n    bool m_lastSceneCut = false;\n    UINT m_lastMaxLocalUnsafe = 0;\n    bool m_haveTelemetry = false;\n""",
    """    UINT m_lastUnsafeCount = 0;\n    bool m_lastSceneCut = false;\n    UINT m_lastMaxLocalUnsafe = 0;\n    UINT m_lastFieldBadCount = 0;\n    bool m_haveTelemetry = false;\n""",
    "Header adaptive telemetry",
)
header = replace_exact(
    header,
    """    CComPtr<ID3D11Buffer> m_denseParameters;\n    CComPtr<ID3D11Buffer> m_warpParameters;\n""",
    """    CComPtr<ID3D11Buffer> m_denseParameters;\n    CComPtr<ID3D11Buffer> m_splatParameters;\n    CComPtr<ID3D11Buffer> m_resolveParameters;\n    CComPtr<ID3D11Buffer> m_warpParameters;\n""",
    "Header adaptive constant buffers",
)
write("NvidiaOpticalFlowDenseSynthesizer.h", header)


# ---------------------------------------------------------------------------
# C++ resource and dispatch plumbing.
# ---------------------------------------------------------------------------
cpp = read("NvidiaOpticalFlowDenseSynthesizer.cpp")
cpp = replace_exact(
    cpp,
    """#include \"NvidiaOpticalFlowDenseUpsampleBytecode.h\"\n#include \"NvidiaOpticalFlowDenseWarpBytecode.h\"\n""",
    """#include \"NvidiaOpticalFlowDenseUpsampleBytecode.h\"\n#include \"NvidiaOpticalFlowAdaptiveSplatScatterBytecode.h\"\n#include \"NvidiaOpticalFlowAdaptiveSplatResolveBytecode.h\"\n#include \"NvidiaOpticalFlowDenseWarpBytecode.h\"\n""",
    "C++ adaptive bytecode includes",
)
cpp = replace_exact(
    cpp,
    """    const std::array<ID3D11ShaderResourceView*, 5> nullSrvs = {};\n    const std::array<ID3D11UnorderedAccessView*, 4> nullUavs = {};\n""",
    """    const std::array<ID3D11ShaderResourceView*, 8> nullSrvs = {};\n    const std::array<ID3D11UnorderedAccessView*, 8> nullUavs = {};\n""",
    "C++ unbind range",
)
cpp = replace_exact(
    cpp,
    """        !CreateShader(device, g_NvofDenseUpsampleBytecode, sizeof(g_NvofDenseUpsampleBytecode),\n            m_denseShader, status, L\"dense NVOF edge-aware upsample\") ||\n        !CreateShader(device, g_NvofDenseWarpBytecode, sizeof(g_NvofDenseWarpBytecode),\n            m_warpShader, status, L\"dense NVOF midpoint warp\")) {\n""",
    """        !CreateShader(device, g_NvofDenseUpsampleBytecode, sizeof(g_NvofDenseUpsampleBytecode),\n            m_denseShader, status, L\"dense NVOF edge-aware upsample\") ||\n        !CreateShader(device, g_NvofAdaptiveSplatScatterBytecode, sizeof(g_NvofAdaptiveSplatScatterBytecode),\n            m_splatShader, status, L\"dense NVOF adaptive splat scatter\") ||\n        !CreateShader(device, g_NvofAdaptiveSplatResolveBytecode, sizeof(g_NvofAdaptiveSplatResolveBytecode),\n            m_resolveShader, status, L\"dense NVOF adaptive splat resolve\") ||\n        !CreateShader(device, g_NvofDenseWarpBytecode, sizeof(g_NvofDenseWarpBytecode),\n            m_warpShader, status, L\"dense NVOF midpoint warp\")) {\n""",
    "C++ adaptive shader creation",
)
cpp = replace_exact(
    cpp,
    """    D3D11_TEXTURE2D_DESC telemetryDesc = qualityDesc;\n""",
    """    const UINT splatElementCount = flowWidth * flowHeight * 6u;\n    D3D11_BUFFER_DESC splatDesc = {};\n    splatDesc.ByteWidth = splatElementCount * sizeof(INT);\n    splatDesc.Usage = D3D11_USAGE_DEFAULT;\n    splatDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;\n    splatDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;\n    splatDesc.StructureByteStride = sizeof(INT);\n    hr = device->CreateBuffer(&splatDesc, nullptr, &m_splatAccumBuffer);\n    if (FAILED(hr)) {\n        status = std::format(L\"CreateBuffer(dense NVOF adaptive splat accumulator) failed ({})\", HR2Str(hr));\n        Reset();\n        return false;\n    }\n\n    D3D11_SHADER_RESOURCE_VIEW_DESC splatSrvDesc = {};\n    splatSrvDesc.Format = DXGI_FORMAT_UNKNOWN;\n    splatSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;\n    splatSrvDesc.Buffer.FirstElement = 0;\n    splatSrvDesc.Buffer.NumElements = splatElementCount;\n    hr = device->CreateShaderResourceView(m_splatAccumBuffer, &splatSrvDesc, &m_splatAccumView);\n    if (FAILED(hr)) {\n        status = std::format(L\"CreateShaderResourceView(dense NVOF adaptive splat accumulator) failed ({})\", HR2Str(hr));\n        Reset();\n        return false;\n    }\n\n    D3D11_UNORDERED_ACCESS_VIEW_DESC splatUavDesc = {};\n    splatUavDesc.Format = DXGI_FORMAT_UNKNOWN;\n    splatUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;\n    splatUavDesc.Buffer.FirstElement = 0;\n    splatUavDesc.Buffer.NumElements = splatElementCount;\n    hr = device->CreateUnorderedAccessView(m_splatAccumBuffer, &splatUavDesc, &m_splatAccumUav);\n    if (FAILED(hr)) {\n        status = std::format(L\"CreateUnorderedAccessView(dense NVOF adaptive splat accumulator) failed ({})\", HR2Str(hr));\n        Reset();\n        return false;\n    }\n\n    D3D11_TEXTURE2D_DESC resolvedDesc = {};\n    resolvedDesc.Width = flowWidth;\n    resolvedDesc.Height = flowHeight;\n    resolvedDesc.MipLevels = 1;\n    resolvedDesc.ArraySize = 2;\n    resolvedDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;\n    resolvedDesc.SampleDesc.Count = 1;\n    resolvedDesc.Usage = D3D11_USAGE_DEFAULT;\n    resolvedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;\n    hr = device->CreateTexture2D(&resolvedDesc, nullptr, &m_resolvedSplatTexture);\n    if (FAILED(hr)) {\n        status = std::format(L\"CreateTexture2D(dense NVOF adaptive splat map) failed ({})\", HR2Str(hr));\n        Reset();\n        return false;\n    }\n\n    D3D11_SHADER_RESOURCE_VIEW_DESC resolvedSrvDesc = {};\n    resolvedSrvDesc.Format = resolvedDesc.Format;\n    resolvedSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;\n    resolvedSrvDesc.Texture2DArray.MostDetailedMip = 0;\n    resolvedSrvDesc.Texture2DArray.MipLevels = 1;\n    resolvedSrvDesc.Texture2DArray.FirstArraySlice = 0;\n    resolvedSrvDesc.Texture2DArray.ArraySize = 2;\n    hr = device->CreateShaderResourceView(m_resolvedSplatTexture, &resolvedSrvDesc, &m_resolvedSplatView);\n    if (FAILED(hr)) {\n        status = std::format(L\"CreateShaderResourceView(dense NVOF adaptive splat map) failed ({})\", HR2Str(hr));\n        Reset();\n        return false;\n    }\n\n    D3D11_UNORDERED_ACCESS_VIEW_DESC resolvedUavDesc = {};\n    resolvedUavDesc.Format = resolvedDesc.Format;\n    resolvedUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;\n    resolvedUavDesc.Texture2DArray.MipSlice = 0;\n    resolvedUavDesc.Texture2DArray.FirstArraySlice = 0;\n    resolvedUavDesc.Texture2DArray.ArraySize = 2;\n    hr = device->CreateUnorderedAccessView(m_resolvedSplatTexture, &resolvedUavDesc, &m_resolvedSplatUav);\n    if (FAILED(hr)) {\n        status = std::format(L\"CreateUnorderedAccessView(dense NVOF adaptive splat map) failed ({})\", HR2Str(hr));\n        Reset();\n        return false;\n    }\n\n    D3D11_TEXTURE2D_DESC telemetryDesc = qualityDesc;\n""",
    "C++ adaptive resources",
)
cpp = replace_exact(
    cpp,
    """        !CreateConstantBuffer<DenseParameters>(device, m_denseParameters, status, L\"dense NVOF upsample params\") ||\n        !CreateConstantBuffer<WarpParameters>(device, m_warpParameters, status, L\"dense NVOF warp params\")) {\n""",
    """        !CreateConstantBuffer<DenseParameters>(device, m_denseParameters, status, L\"dense NVOF upsample params\") ||\n        !CreateConstantBuffer<SplatParameters>(device, m_splatParameters, status, L\"dense NVOF adaptive splat params\") ||\n        !CreateConstantBuffer<ResolveParameters>(device, m_resolveParameters, status, L\"dense NVOF adaptive resolve params\") ||\n        !CreateConstantBuffer<WarpParameters>(device, m_warpParameters, status, L\"dense NVOF warp params\")) {\n""",
    "C++ adaptive constant buffers",
)
cpp = replace_exact(
    cpp,
    """    m_linearSampler.Release();\n    m_warpParameters.Release();\n    m_denseParameters.Release();\n""",
    """    m_linearSampler.Release();\n    m_warpParameters.Release();\n    m_resolveParameters.Release();\n    m_splatParameters.Release();\n    m_denseParameters.Release();\n""",
    "C++ adaptive reset constant buffers",
)
cpp = replace_exact(
    cpp,
    """    m_regionRejectUav.Release();\n    m_regionRejectView.Release();\n    m_regionRejectTexture.Release();\n""",
    """    m_regionRejectUav.Release();\n    m_regionRejectView.Release();\n    m_regionRejectTexture.Release();\n    m_resolvedSplatUav.Release();\n    m_resolvedSplatView.Release();\n    m_resolvedSplatTexture.Release();\n    m_splatAccumUav.Release();\n    m_splatAccumView.Release();\n    m_splatAccumBuffer.Release();\n""",
    "C++ adaptive reset resources",
)
cpp = replace_exact(
    cpp,
    """    m_lastMaxLocalUnsafe = 0;\n    m_haveTelemetry = false;\n""",
    """    m_lastMaxLocalUnsafe = 0;\n    m_lastFieldBadCount = 0;\n    m_haveTelemetry = false;\n""",
    "C++ adaptive reset telemetry",
)
cpp = replace_exact(
    cpp,
    """    m_warpShader.Release();\n    m_denseShader.Release();\n""",
    """    m_warpShader.Release();\n    m_resolveShader.Release();\n    m_splatShader.Release();\n    m_denseShader.Release();\n""",
    "C++ adaptive reset shaders",
)
cpp = replace_exact(
    cpp,
    """std::wstring CNvidiaOpticalFlowDenseSynthesizer::GetTelemetryText() const\n{\n    if (!m_haveTelemetry || !m_flowWidth || !m_flowHeight) {\n        return L\"quality telemetry warming up\";\n    }\n    const UINT cellCount = m_flowWidth * m_flowHeight;\n    const double badPercent = 100.0 * static_cast<double>(m_lastUnsafeCount) /\n        std::max(1u, cellCount);\n    return std::format(\n        L\"cut={}, bad {:.1f}% ({}/{}), guard=local-photo, seeds=asym-backfill, worst7x7 {}/49, would8={}, would18={}\",\n        m_lastSceneCut ? L\"yes\" : L\"no\",\n        badPercent, m_lastUnsafeCount, cellCount, m_lastMaxLocalUnsafe,\n        m_lastMaxLocalUnsafe >= 8 ? L\"yes\" : L\"no\",\n        m_lastMaxLocalUnsafe >= 18 ? L\"yes\" : L\"no\");\n}\n""",
    """std::wstring CNvidiaOpticalFlowDenseSynthesizer::GetTelemetryText() const\n{\n    if (!m_haveTelemetry || !m_flowWidth || !m_flowHeight) {\n        return L\"quality telemetry warming up\";\n    }\n    const UINT cellCount = m_flowWidth * m_flowHeight;\n    const double badPercent = 100.0 * static_cast<double>(m_lastUnsafeCount) /\n        std::max(1u, cellCount);\n    const UINT borderX = std::max(1u, static_cast<UINT>(static_cast<double>(m_flowWidth) * 0.04));\n    const UINT borderY = std::max(1u, static_cast<UINT>(static_cast<double>(m_flowHeight) * 0.04));\n    const UINT interiorWidth = m_flowWidth > 2u * borderX ? m_flowWidth - 2u * borderX : 1u;\n    const UINT interiorHeight = m_flowHeight > 2u * borderY ? m_flowHeight - 2u * borderY : 1u;\n    const UINT interiorCount = std::max(1u, interiorWidth * interiorHeight);\n    const double fieldPercent = 100.0 * static_cast<double>(m_lastFieldBadCount) / interiorCount;\n    return std::format(\n        L\"cut={}, bad {:.1f}% ({}/{}), field-q {:.1f}% ({}/{}), guard=local-photo+adaptive-splat-v12, seeds=asym-backfill, worst7x7 {}/49\",\n        m_lastSceneCut ? L\"yes\" : L\"no\",\n        badPercent, m_lastUnsafeCount, cellCount,\n        fieldPercent, m_lastFieldBadCount, interiorCount, m_lastMaxLocalUnsafe);\n}\n""",
    "C++ adaptive telemetry text",
)
cpp = replace_exact(
    cpp,
    """            !m_seedShader || !m_repairShader || !m_regionGateShader || !m_jumpShader || !m_denseShader || !m_warpShader ||\n            !m_qualityUav || !m_qualityView || !m_unsafeCellUav || !m_unsafeCellView ||\n            !m_repairUavs[0] || !m_repairViews[0] || !m_repairUavs[1] || !m_repairViews[1] ||\n            !m_regionRejectUav || !m_regionRejectView) {\n""",
    """            !m_seedShader || !m_repairShader || !m_regionGateShader || !m_jumpShader || !m_denseShader ||\n            !m_splatShader || !m_resolveShader || !m_warpShader ||\n            !m_qualityUav || !m_qualityView || !m_unsafeCellUav || !m_unsafeCellView ||\n            !m_repairUavs[0] || !m_repairViews[0] || !m_repairUavs[1] || !m_repairViews[1] ||\n            !m_regionRejectUav || !m_regionRejectView || !m_splatAccumUav || !m_splatAccumView ||\n            !m_resolvedSplatUav || !m_resolvedSplatView) {\n""",
    "C++ adaptive resource validation",
)
cpp = replace_exact(
    cpp,
    """    context->ClearUnorderedAccessViewUint(m_qualityUav, zero);\n    context->ClearUnorderedAccessViewUint(m_regionRejectUav, zero);\n""",
    """    context->ClearUnorderedAccessViewUint(m_qualityUav, zero);\n    context->ClearUnorderedAccessViewUint(m_regionRejectUav, zero);\n    context->ClearUnorderedAccessViewUint(m_splatAccumUav, zero);\n""",
    "C++ adaptive clears",
)
cpp = replace_exact(
    cpp,
    """    const std::array<ID3D11UnorderedAccessView*, 4> seedOutputs = {\n        m_seedUavs[0], m_qualityUav, m_unsafeCellUav, m_repairUavs[0],\n    };\n    context->CSSetShader(m_seedShader, nullptr, 0);\n    context->CSSetConstantBuffers(0, 1, &seedBuffer);\n    context->CSSetShaderResources(0, static_cast<UINT>(seedInputs.size()), seedInputs.data());\n    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(seedOutputs.size()), seedOutputs.data(), nullptr);\n""",
    """    const std::array<ID3D11UnorderedAccessView*, 5> seedOutputs = {\n        m_seedUavs[0], m_qualityUav, m_unsafeCellUav, m_repairUavs[0], m_regionRejectUav,\n    };\n    ID3D11SamplerState* seedSampler = m_linearSampler;\n    context->CSSetShader(m_seedShader, nullptr, 0);\n    context->CSSetConstantBuffers(0, 1, &seedBuffer);\n    context->CSSetSamplers(0, 1, &seedSampler);\n    context->CSSetShaderResources(0, static_cast<UINT>(seedInputs.size()), seedInputs.data());\n    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(seedOutputs.size()), seedOutputs.data(), nullptr);\n""",
    "C++ adaptive Seed bindings",
)
cpp = replace_exact(
    cpp,
    """    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);\n    UnbindCompute(context);\n\n    // Async diagnostics: read a staging slot from three submissions ago with\n""",
    """    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);\n    UnbindCompute(context);\n\n    const SplatParameters splatValues = {\n        m_flowWidth, m_flowHeight, 4.0f, midpointTime,\n    };\n    context->UpdateSubresource(m_splatParameters, 0, nullptr, &splatValues, 0, 0);\n    ID3D11Buffer* splatBuffer = m_splatParameters;\n    const std::array<ID3D11ShaderResourceView*, 5> splatInputs = {\n        forwardFlowBtoA, backwardFlowAtoB, m_unsafeCellView, m_qualityView, m_regionRejectView,\n    };\n    ID3D11UnorderedAccessView* splatOutput = m_splatAccumUav;\n    context->CSSetShader(m_splatShader, nullptr, 0);\n    context->CSSetConstantBuffers(0, 1, &splatBuffer);\n    context->CSSetShaderResources(0, static_cast<UINT>(splatInputs.size()), splatInputs.data());\n    context->CSSetUnorderedAccessViews(0, 1, &splatOutput, nullptr);\n    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);\n    UnbindCompute(context);\n\n    const ResolveParameters resolveValues = {\n        m_flowWidth, m_flowHeight, 60.0f, 0.0f,\n    };\n    context->UpdateSubresource(m_resolveParameters, 0, nullptr, &resolveValues, 0, 0);\n    ID3D11Buffer* resolveBuffer = m_resolveParameters;\n    const std::array<ID3D11ShaderResourceView*, 3> resolveInputs = {\n        m_splatAccumView, m_unsafeCellView, m_regionRejectView,\n    };\n    ID3D11UnorderedAccessView* resolveOutput = m_resolvedSplatUav;\n    context->CSSetShader(m_resolveShader, nullptr, 0);\n    context->CSSetConstantBuffers(0, 1, &resolveBuffer);\n    context->CSSetShaderResources(0, static_cast<UINT>(resolveInputs.size()), resolveInputs.data());\n    context->CSSetUnorderedAccessViews(0, 1, &resolveOutput, nullptr);\n    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);\n    UnbindCompute(context);\n\n    // Async diagnostics: read a staging slot from three submissions ago with\n""",
    "C++ adaptive dispatches",
)
cpp = replace_exact(
    cpp,
    """                m_lastSceneCut = (packedQuality & 0x80000000u) != 0u;\n                m_lastUnsafeCount = packedQuality & 0x7fffffffu;\n                m_lastMaxLocalUnsafe = *static_cast<const UINT*>(regionMapped.pData);\n                m_haveTelemetry = true;\n""",
    """                m_lastSceneCut = (packedQuality & 0x80000000u) != 0u;\n                m_lastUnsafeCount = packedQuality & 0x7fffffffu;\n                const UINT packedRegion = *static_cast<const UINT*>(regionMapped.pData);\n                m_lastMaxLocalUnsafe = packedRegion & 0x3fu;\n                m_lastFieldBadCount = packedRegion >> 6u;\n                m_haveTelemetry = true;\n""",
    "C++ adaptive telemetry decode",
)
cpp = replace_exact(
    cpp,
    """    const WarpParameters warpValues = {\n        m_frameWidth, m_frameHeight, m_flowWidth * m_flowHeight, 2.0f,\n        midpointTime, 4.0f, {0.0f, 0.0f},\n    };\n""",
    """    const WarpParameters warpValues = {\n        m_frameWidth, m_frameHeight, m_flowWidth, m_flowHeight,\n        midpointTime, 4.0f, {0.0f, 0.0f},\n    };\n""",
    "C++ adaptive Warp params",
)
cpp = replace_exact(
    cpp,
    """    const std::array<ID3D11ShaderResourceView*, 5> warpInputs = {\n        previousFrame, nextFrame, m_denseFlowView, m_qualityView, m_repairViews[1],\n    };\n""",
    """    const std::array<ID3D11ShaderResourceView*, 6> warpInputs = {\n        previousFrame, nextFrame, m_denseFlowView, m_qualityView, m_repairViews[1], m_resolvedSplatView,\n    };\n""",
    "C++ adaptive Warp inputs",
)
write("NvidiaOpticalFlowDenseSynthesizer.cpp", cpp)

print("Applied NVOF adaptive-splat V1.2 build-time source patch.")
