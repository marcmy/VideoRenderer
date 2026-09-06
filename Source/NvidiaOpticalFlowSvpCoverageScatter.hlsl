Texture2D<int2> FlowBtoA : register(t0);
Texture2D<int2> FlowAtoB : register(t1);
StructuredBuffer<uint> Control : register(t2);

RWStructuredBuffer<uint> AccumA : register(u0);
RWStructuredBuffer<uint> AccumB : register(u1);

cbuffer CoverageScatterParameters : register(b0)
{
    uint2 FlowSize;
    uint2 Padding;
};

int FloorDiv4(int value)
{
    return value >= 0 ? value / 4 : -((-value + 3) / 4);
}

void AddPoint(RWStructuredBuffer<uint> accum, int x, int y, int weight)
{
    if (weight <= 0 || x < 0 || y < 0 || x >= (int)FlowSize.x || y >= (int)FlowSize.y) {
        return;
    }
    uint ignored;
    InterlockedAdd(accum[(uint)y * FlowSize.x + (uint)x], (uint)weight, ignored);
}

void Splat(RWStructuredBuffer<uint> accum, int2 raw, int2 cell, uint threshold)
{
    // SVP's packed NVOF direction stores raw SHORT2 / 8, marker=4.  Coverage
    // applies phase as stored * phase / (marker * 256).
    int2 stored = raw / 8;
    int2 displacement = (stored * (int)threshold) / 1024;
    int2 shifted = cell * 4 + displacement;

    int left = FloorDiv4(shifted.x);
    int top = FloorDiv4(shifted.y);
    int right = left + 1;
    int bottom = top + 1;
    int nextX = right * 4;
    int nextY = bottom * 4;
    int leftWeight = nextX - shifted.x;
    int topWeight = nextY - shifted.y;
    int rightWeight = shifted.x + 4 - nextX;
    int bottomWeight = shifted.y + 4 - nextY;

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

    // mask A is generated from the opposite A->B field at inverse phase;
    // mask B is generated from the opposite B->A field at forward phase.
    Splat(AccumA, FlowAtoB.Load(int3(cell, 0)), cell, 256u - phase);
    Splat(AccumB, FlowBtoA.Load(int3(cell, 0)), cell, phase);
}
