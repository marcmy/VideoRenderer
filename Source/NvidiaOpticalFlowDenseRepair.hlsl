Texture2D<float4> RepairCandidate : register(t0);
RWTexture2D<float4> RepairField : register(u0);

cbuffer RepairParameters : register(b0)
{
    uint2 FlowSize;
    float FlowSigma;
    float MaskSigma;
    uint FlowRadius;
    uint MaskRadius;
    float MaskDilation;
    float Padding;
};

float4 LoadCandidateClamped(int2 cell)
{
    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);
    return RepairCandidate.Load(int3(cell, 0));
}

float LoadCatastrophic(int2 cell)
{
    if (any(cell < 0) || any(cell >= int2(FlowSize))) return 0.0;
    float encoded = round(RepairCandidate.Load(int3(cell, 0)).w);
    return fmod(encoded, 2.0) >= 1.0 ? 1.0 : 0.0;
}

float LoadUnsupported(int2 cell)
{
    if (any(cell < 0) || any(cell >= int2(FlowSize))) return 0.0;
    float encoded = round(RepairCandidate.Load(int3(cell, 0)).w);
    return encoded >= 2.0 ? 1.0 : 0.0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;

    int2 center = int2(id.xy);

    float2 flowSum = 0.0;
    float weightSum = 0.0;
    float flowDenom = max(2.0 * FlowSigma * FlowSigma, 1.0e-6);

    [loop]
    for (int fy = -int(FlowRadius); fy <= int(FlowRadius); ++fy) {
        [loop]
        for (int fx = -int(FlowRadius); fx <= int(FlowRadius); ++fx) {
            float4 candidate = LoadCandidateClamped(center + int2(fx, fy));
            float dist2 = float(fx * fx + fy * fy);
            float spatialWeight = exp(-dist2 / flowDenom);
            float weight = spatialWeight * max(candidate.z, 1.0e-4);
            flowSum += candidate.xy * weight;
            weightSum += weight;
        }
    }

    float2 repairMotion = weightSum > 1.0e-6
        ? flowSum / weightSum
        : LoadCandidateClamped(center).xy;

    // Build two independently feathered coarse masks. Z retains the existing
    // catastrophic-region repair mask. W marks regions where neither native
    // NVOF direction passed its own consistency check, meaning the dense/JFA
    // motion has no trustworthy source seed and should be treated as unsupported.
    float repairMask = 0.0;
    float unsupportedMask = 0.0;
    float maskDenom = max(2.0 * MaskSigma * MaskSigma, 1.0e-6);
    [loop]
    for (int my = -int(MaskRadius); my <= int(MaskRadius); ++my) {
        [loop]
        for (int mx = -int(MaskRadius); mx <= int(MaskRadius); ++mx) {
            int2 sampleCell = center + int2(mx, my);
            float catastrophic = LoadCatastrophic(sampleCell);
            float unsupported = LoadUnsupported(sampleCell);
            if (catastrophic <= 0.0 && unsupported <= 0.0) continue;

            float distance = length(float2(mx, my));
            float outsideDilation = max(0.0, distance - MaskDilation);
            float feather = exp(
                -(outsideDilation * outsideDilation) / maskDenom);
            if (catastrophic > 0.0) {
                repairMask = max(repairMask, feather);
            }
            if (unsupported > 0.0) {
                unsupportedMask = max(unsupportedMask, feather);
            }
        }
    }

    RepairField[id.xy] = float4(
        repairMotion,
        saturate(repairMask),
        saturate(unsupportedMask));
}
