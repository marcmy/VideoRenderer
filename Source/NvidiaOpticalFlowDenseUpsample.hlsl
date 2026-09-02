Texture2D<float4> NextFrame : register(t0);
Texture2D<int2> ForwardFlowBtoA : register(t1);
Texture2D<int2> BackwardFlowAtoB : register(t2);
Texture2D<uint> SeedMap : register(t3);
RWTexture2D<float2> DenseFlow : register(u0);

cbuffer DenseParameters : register(b0)
{
    uint2 FrameSize;
    uint2 FlowSize;
    float GridSize;
    float SpatialSigma;
    float ColorSigma;
    float InfillSigma;
};

static const uint InvalidSeed = 0xffffffffu;
static const uint BackwardSeedBit = 0x80000000u;

uint2 UnpackSeed(uint packed)
{
    return uint2(packed & 0xffffu, (packed >> 16) & 0x7fffu);
}

bool SeedUsesBackward(uint packed)
{
    return (packed & BackwardSeedBit) != 0u;
}

float2 LoadRawFlow(int2 cell)
{
    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);
    return float2(ForwardFlowBtoA.Load(int3(cell, 0))) / 32.0;
}

float2 LoadBackwardFlow(int2 cell)
{
    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);
    return float2(BackwardFlowAtoB.Load(int3(cell, 0))) / 32.0;
}

float2 LoadSeedFlow(uint packedSeed)
{
    int2 seed = int2(UnpackSeed(packedSeed));
    return SeedUsesBackward(packedSeed)
        ? -LoadBackwardFlow(seed)
        : LoadRawFlow(seed);
}

float2 SampleRawFlow(float2 pixel)
{
    float2 grid = pixel / GridSize;
    int2 base = int2(floor(grid));
    float2 f = frac(grid);
    float2 v00 = LoadRawFlow(base);
    float2 v10 = LoadRawFlow(base + int2(1, 0));
    float2 v01 = LoadRawFlow(base + int2(0, 1));
    float2 v11 = LoadRawFlow(base + int2(1, 1));
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

float3 LoadFrameColor(int2 pixel)
{
    pixel = clamp(pixel, int2(0, 0), int2(FrameSize) - 1);
    return NextFrame.Load(int3(pixel, 0)).rgb;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FrameSize)) return;

    float2 pixel = float2(id.xy);
    float2 grid = pixel / GridSize;
    int2 base = int2(floor(grid));
    float3 guideColor = LoadFrameColor(int2(id.xy));

    float2 flowSum = 0.0;
    float weightSum = 0.0;
    float spatialDenom = max(2.0 * SpatialSigma * SpatialSigma, 1.0e-6);
    float colorDenom = max(2.0 * ColorSigma * ColorSigma, 1.0e-6);
    float infillScale = max(InfillSigma, 1.0e-6);

    [unroll]
    for (int oy = -2; oy <= 2; ++oy) {
        [unroll]
        for (int ox = -2; ox <= 2; ++ox) {
            int2 cell = clamp(base + int2(ox, oy), int2(0, 0), int2(FlowSize) - 1);
            uint packedSeed = SeedMap.Load(int3(cell, 0));
            if (packedSeed == InvalidSeed) continue;

            uint2 seed = UnpackSeed(packedSeed);
            float2 cellDelta = grid - float2(cell);
            float spatialWeight = exp(-dot(cellDelta, cellDelta) / spatialDenom);

            // The candidate motion belongs to the propagated valid seed, not to
            // the invalid cell through which that seed happened to arrive. Guide
            // motion-layer selection with the seed's actual source appearance.
            int2 seedPixel = clamp(int2(float2(seed) * GridSize), int2(0, 0), int2(FrameSize) - 1);
            float3 colorDelta = guideColor - LoadFrameColor(seedPixel);
            float colorWeight = exp(-dot(colorDelta, colorDelta) / colorDenom);

            float infillDistance = length(float2(seed) - float2(cell));
            float infillWeight = exp(-infillDistance / infillScale);
            float weight = spatialWeight * colorWeight * infillWeight;

            flowSum += LoadSeedFlow(packedSeed) * weight;
            weightSum += weight;
        }
    }

    DenseFlow[id.xy] = weightSum > 1.0e-5
        ? flowSum / weightSum
        : SampleRawFlow(pixel);
}
