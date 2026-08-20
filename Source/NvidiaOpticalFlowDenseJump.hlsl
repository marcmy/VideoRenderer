Texture2D<uint> SeedInput : register(t0);
RWTexture2D<uint> SeedOutput : register(u0);

cbuffer JumpParameters : register(b0)
{
    uint2 FlowSize;
    uint JumpStep;
    uint Padding;
};

static const uint InvalidSeed = 0xffffffffu;

uint2 UnpackSeed(uint packed)
{
    // Bit 31 marks an A->B-derived backfill seed; it is not part of Y.
    return uint2(packed & 0xffffu, (packed >> 16) & 0x7fffu);
}

float SeedDistanceSquared(int2 cell, uint packed)
{
    if (packed == InvalidSeed) return 3.402823466e+38F;
    float2 delta = float2(UnpackSeed(packed)) - float2(cell);
    return dot(delta, delta);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;

    int2 cell = int2(id.xy);
    uint bestSeed = SeedInput.Load(int3(cell, 0));
    float bestDistance = SeedDistanceSquared(cell, bestSeed);

    [unroll]
    for (int oy = -1; oy <= 1; ++oy) {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox) {
            int2 candidateCell = clamp(
                cell + int2(ox, oy) * int(JumpStep),
                int2(0, 0), int2(FlowSize) - 1);
            uint candidateSeed = SeedInput.Load(int3(candidateCell, 0));
            float candidateDistance = SeedDistanceSquared(cell, candidateSeed);
            if (candidateDistance < bestDistance ||
                    (candidateDistance == bestDistance && candidateSeed < bestSeed)) {
                bestDistance = candidateDistance;
                bestSeed = candidateSeed;
            }
        }
    }

    SeedOutput[id.xy] = bestSeed;
}
