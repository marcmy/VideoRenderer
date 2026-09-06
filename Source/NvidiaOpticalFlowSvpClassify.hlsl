Texture2D<int2> CurrentFlowAtoB : register(t0);
Texture2D<float> PreviousY : register(t1);
Texture2D<float> NextY : register(t2);
Texture1D<uint> PairLumaLut : register(t3);

RWStructuredBuffer<uint> Counters : register(u0);

cbuffer ClassifyParameters : register(b0)
{
    uint2 FlowSize;
    uint2 FrameSize;
    uint BorderX;
    uint BorderY;
    uint2 Padding;
};

uint LoadY(Texture2D<float> image, int2 p)
{
    p = clamp(p, int2(0, 0), int2(FrameSize) - 1);
    return (uint)round(saturate(image.Load(int3(p, 0))) * 255.0);
}

uint BlockLuma(Texture2D<float> image, int2 base)
{
    uint sum = 0;
    [unroll]
    for (int y = 0; y < 4; ++y) {
        [unroll]
        for (int x = 0; x < 4; ++x) {
            sum += LoadY(image, base + int2(x, y));
        }
    }
    return sum >> 4;
}

uint DirectionSad(int2 base, int2 displacement)
{
    uint sad = 0;
    [unroll]
    for (int y = 0; y < 4; ++y) {
        [unroll]
        for (int x = 0; x < 4; ++x) {
            int2 p = base + int2(x, y);
            uint a = LoadY(PreviousY, p);
            uint b = LoadY(NextY, p + displacement);
            sad += a > b ? a - b : b - a;
        }
    }
    return sad;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;
    if (id.x < BorderX || id.y < BorderY ||
        id.x >= FlowSize.x - BorderX || id.y >= FlowSize.y - BorderY) {
        return;
    }

    int2 cell = int2(id.xy);
    int2 raw = CurrentFlowAtoB.Load(int3(cell, 0));
    // NVOF SHORT2 is S10.5. Proprietary modern confidence uses an integer
    // displacement truncated toward zero: raw / 32.
    int2 displacement = raw / 32;
    int2 base = cell * 4;

    uint lumaA = BlockLuma(PreviousY, base);
    uint lumaB = BlockLuma(NextY, base);
    uint pairLuma = PairLumaLut.Load(int2(min(lumaA + lumaB, 510u), 0));
    pairLuma = max(pairLuma, 1u);

    uint score = DirectionSad(base, displacement) & 0x00ffffffu;
    uint q = (score * 255u) / pairLuma;

    // Exact classifier buckets. The CPU loop's "skip the first 2/3 zero
    // blocks" is order-independent for classification because q<zero cells
    // never contribute to the severity buckets. The control pass therefore
    // reconstructs considered = interior - min(low, 2*full/3).
    if (q < 200u) InterlockedAdd(Counters[0], 1u);
    else if (q >= 4000u) InterlockedAdd(Counters[3], 1u);
    else if (q >= 2800u) InterlockedAdd(Counters[2], 1u);
    else if (q >= 1600u) InterlockedAdd(Counters[1], 1u);
}
