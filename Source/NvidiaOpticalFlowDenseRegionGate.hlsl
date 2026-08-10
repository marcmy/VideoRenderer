Texture2D<uint> UnsafeCellMap : register(t0);
RWTexture2D<uint> RegionReject : register(u0);

cbuffer RegionGateParameters : register(b0)
{
    uint2 FlowSize;
    uint MinUnsafeCells;
    uint Radius;
};

uint LoadUnsafe(int2 cell)
{
    if (any(cell < 0) || any(cell >= int2(FlowSize))) return 0u;
    return UnsafeCellMap.Load(int3(cell, 0)) != 0u ? 1u : 0u;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;

    int2 center = int2(id.xy);
    if (LoadUnsafe(center) == 0u) return;

    uint unsafeCount = 0u;
    [loop]
    for (int y = -int(Radius); y <= int(Radius); ++y) {
        [loop]
        for (int x = -int(Radius); x <= int(Radius); ++x) {
            unsafeCount += LoadUnsafe(center + int2(x, y));
        }
    }

    // Do not splice real-frame patches into the synthetic image. If a genuine
    // local cluster of catastrophic NVOF cells exists, reject the entire
    // inserted midpoint so spatial coherence is preserved.
    if (unsafeCount >= MinUnsafeCells) {
        InterlockedOr(RegionReject[uint2(0, 0)], 1u);
    }
}
