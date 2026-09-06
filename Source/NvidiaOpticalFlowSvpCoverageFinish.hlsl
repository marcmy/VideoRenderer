StructuredBuffer<uint> AccumA : register(t0);
StructuredBuffer<uint> AccumB : register(t1);

RWStructuredBuffer<uint> MaskA : register(u0);
RWStructuredBuffer<uint> MaskB : register(u1);

cbuffer CoverageFinishParameters : register(b0)
{
    uint2 FlowSize;
    uint CoverPercent;
    uint Padding;
};

uint Window3x3(StructuredBuffer<uint> accum, int2 cell)
{
    uint sum = 0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            int2 p = cell + int2(x, y);
            if (p.x >= 0 && p.y >= 0 && p.x < (int)FlowSize.x && p.y < (int)FlowSize.y) {
                sum += accum[(uint)p.y * FlowSize.x + (uint)p.x];
            }
        }
    }
    return sum;
}

uint FinishMask(uint sum)
{
    const uint area = 16u;
    uint covered = sum >> 3;
    uint remaining = covered >= area ? 0u : area - covered;
    uint value = (remaining * CoverPercent * 256u) / (100u * area);
    return min(value, 255u);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;
    uint index = id.y * FlowSize.x + id.x;
    MaskA[index] = FinishMask(Window3x3(AccumA, int2(id.xy)));
    MaskB[index] = FinishMask(Window3x3(AccumB, int2(id.xy)));
}
