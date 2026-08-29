StructuredBuffer<int> SplatAccum : register(t0);
Texture2D<uint> PackedCellMetadata : register(t1);

RWTexture2D<float4> ResolvedPrevious : register(u0);
RWTexture2D<float4> ResolvedNext : register(u1);

cbuffer ResolveParameters : register(b0)
{
    uint2 FlowSize;
    float WeightScale;
    float Padding;
};

float3 ResolveSide(uint cellIndex, uint offset)
{
    uint base = cellIndex * 6u + offset;
    int sumX = SplatAccum[base + 0u];
    int sumY = SplatAccum[base + 1u];
    int sumW = SplatAccum[base + 2u];
    if (sumW <= 0) return 0.0;
    float invW = 1.0 / float(sumW);
    return float3(float(sumX) * invW, float(sumY) * invW,
        saturate(float(sumW) / max(WeightScale, 1.0)));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;

    uint cellIndex = id.y * FlowSize.x + id.x;
    float3 previous = ResolveSide(cellIndex, 0u);
    float3 next = ResolveSide(cellIndex, 3u);

    uint packed = PackedCellMetadata.Load(int3(id.xy, 0));
    float qBtoA = float(packed & 0x3ffu) * 8.0;
    float qAtoB = float((packed >> 10u) & 0x3ffu) * 8.0;
    bool inBoundsBtoA = (packed & (1u << 28u)) != 0u;
    bool inBoundsAtoB = (packed & (1u << 29u)) != 0u;
    float combinedQ = 0.5 * (
        (inBoundsBtoA ? qBtoA : 0.0)
        + (inBoundsAtoB ? qAtoB : 0.0));

    ResolvedPrevious[id.xy] = float4(previous.xy, previous.z, combinedQ);
    ResolvedNext[id.xy] = float4(next.xy, next.z, 0.0);
}
