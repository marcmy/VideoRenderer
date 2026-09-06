Texture2D<int2> FlowBtoA : register(t0);
Texture2D<int2> FlowAtoB : register(t1);
StructuredBuffer<uint> Control : register(t2);

RWStructuredBuffer<uint> AccumA : register(u0);
RWStructuredBuffer<uint> AccumB : register(u1);

cbuffer CoverageScatterParameters : register(b0)
{
    uint2 FlowSize;
    uint BlockSize;
    uint SourceScale;
    uint VectorPrecision;
    uint3 Padding;
};

int FloorDiv(int value, int divisor)
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

void AddPoint(RWStructuredBuffer<uint> accum, int x, int y, int weight)
{
    if (weight <= 0 || x < 0 || y < 0 || x >= (int)FlowSize.x || y >= (int)FlowSize.y) return;
    uint ignored;
    InterlockedAdd(accum[(uint)y * FlowSize.x + (uint)x], (uint)weight, ignored);
}

int2 PackedFullResolutionMotion(int2 raw)
{
    // Proprietary reduced-source packing:
    // packed = trunc(raw_S10.5 * scale * precision / 32).
    return (raw * (int)(SourceScale * VectorPrecision)) / 32;
}

void Splat(RWStructuredBuffer<uint> accum, int2 raw, int2 cell, uint threshold)
{
    int2 packed = PackedFullResolutionMotion(raw);
    int2 displacement = (packed * (int)threshold) / (int)(VectorPrecision * 256u);
    int2 shifted = cell * (int)BlockSize + displacement;

    int left = FloorDiv(shifted.x, (int)BlockSize);
    int top = FloorDiv(shifted.y, (int)BlockSize);
    int right = left + 1;
    int bottom = top + 1;
    int nextX = right * (int)BlockSize;
    int nextY = bottom * (int)BlockSize;
    int leftWeight = nextX - shifted.x;
    int topWeight = nextY - shifted.y;
    int rightWeight = shifted.x + (int)BlockSize - nextX;
    int bottomWeight = shifted.y + (int)BlockSize - nextY;

    AddPoint(accum, left, top, leftWeight * topWeight);
    AddPoint(accum, right, top, rightWeight * topWeight);
    AddPoint(accum, left, bottom, leftWeight * bottomWeight);
    AddPoint(accum, right, bottom, rightWeight * bottomWeight);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;
    int2 cell = int2(id.xy);
    uint phase = Control[2];
    Splat(AccumA, FlowAtoB.Load(int3(cell, 0)), cell, 256u - phase);
    Splat(AccumB, FlowBtoA.Load(int3(cell, 0)), cell, phase);
}
