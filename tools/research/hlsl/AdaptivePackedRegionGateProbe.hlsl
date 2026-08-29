Texture2D<uint> PackedCellMetadata : register(t0);
RWTexture2D<uint> PackedRegionStats : register(u0);

cbuffer RegionGateProbeParameters : register(b0)
{
    uint2 FlowSize;
    uint Radius;
    uint Padding;
};

static const uint CatastrophicBit = 1u << 30u;
static const uint LowTelemetryMask = 63u;

uint LoadCatastrophic(int2 cell)
{
    if (any(cell < 0) || any(cell >= int2(FlowSize))) return 0u;
    uint packed = PackedCellMetadata.Load(int3(cell, 0));
    return (packed & CatastrophicBit) != 0u ? 1u : 0u;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;
    int2 center = int2(id.xy);
    if (LoadCatastrophic(center) == 0u) return;

    uint unsafeCount = 0u;
    [loop]
    for (int y = -int(Radius); y <= int(Radius); ++y) {
        [loop]
        for (int x = -int(Radius); x <= int(Radius); ++x) {
            unsafeCount += LoadCatastrophic(center + int2(x, y));
        }
    }

    // Seed has already finished writing the upper field-count bits. Preserve
    // those stable bits while atomically maximizing only the low telemetry bits.
    uint packedNow = PackedRegionStats.Load(int3(0, 0, 0));
    uint base = packedNow & ~LowTelemetryMask;
    uint candidate = base | min(unsafeCount, LowTelemetryMask);
    InterlockedMax(PackedRegionStats[uint2(0, 0)], candidate);
}
