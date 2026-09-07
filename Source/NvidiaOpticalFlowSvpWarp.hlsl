Texture2D<float4> PreviousFrame : register(t0);
Texture2D<float4> NextFrame : register(t1);
Texture2D<int2> FlowBtoA : register(t2);
Texture2D<int2> FlowAtoB : register(t3);
StructuredBuffer<uint> MaskA : register(t4);
StructuredBuffer<uint> MaskB : register(t5);
StructuredBuffer<uint> Control : register(t6);

RWTexture2D<float4> OutputFrame : register(u0);

cbuffer WarpParameters : register(b0)
{
    uint2 FrameSize;
    uint2 FlowSize;
    uint BlockSize;
    uint SourceScale;
    uint VectorPrecision;
    uint Padding;
};

int2 ClampVectorToFrame(int2 value, int2 cell)
{
    int2 pos = cell * (int)BlockSize;
    if (value.x + pos.x < 0) value.x = -pos.x;
    else if (value.x + pos.x + (int)BlockSize > (int)FrameSize.x)
        value.x = max((int)FrameSize.x - (int)BlockSize - pos.x, 0);
    if (value.y + pos.y < 0) value.y = -pos.y;
    else if (value.y + pos.y + (int)BlockSize > (int)FrameSize.y)
        value.y = max((int)FrameSize.y - (int)BlockSize - pos.y, 0);
    return value;
}

int2 MotionAt(Texture2D<int2> flow, int2 cell, uint threshold)
{
    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);
    int2 raw = flow.Load(int3(cell, 0));
    // Reduced-source NVOF vectors are packed exactly as reconstructed from
    // SVPflow, then divided by the corresponding precision in the renderer.
    int2 packed = (raw * (int)(SourceScale * VectorPrecision)) / 32;
    int2 motionVector = packed / (int)VectorPrecision;
    motionVector = ClampVectorToFrame(motionVector, cell);
    return (motionVector * (int)threshold) / 256;
}

void GridPosition(uint2 pixel, out int2 baseCell, out int2 nextCell, out int2 fraction)
{
    int2 p = int2(pixel) - int2((int)BlockSize / 2, (int)BlockSize / 2);
    baseCell = int2(0, 0);
    fraction = int2(0, 0);
    if (p.x >= 0) { baseCell.x = p.x / (int)BlockSize; fraction.x = p.x % (int)BlockSize; }
    if (p.y >= 0) { baseCell.y = p.y / (int)BlockSize; fraction.y = p.y % (int)BlockSize; }
    baseCell = clamp(baseCell, int2(0, 0), int2(FlowSize) - 1);
    nextCell = min(baseCell + 1, int2(FlowSize) - 1);
}

int InterpBlock(int a, int b, int fraction)
{
    // Signed integer division intentionally preserves SVP's truncation toward
    // zero for all supported effective grids (4/8/16/24/32).
    return (((int)BlockSize - fraction) * a + fraction * b) / (int)BlockSize;
}

int2 InterpolateMotion(Texture2D<int2> flow, uint2 pixel, uint threshold)
{
    int2 c0, c1, f;
    GridPosition(pixel, c0, c1, f);
    int2 v00 = MotionAt(flow, c0, threshold);
    int2 v10 = MotionAt(flow, int2(c1.x, c0.y), threshold);
    int2 v01 = MotionAt(flow, int2(c0.x, c1.y), threshold);
    int2 v11 = MotionAt(flow, c1, threshold);
    int2 left = int2(InterpBlock(v00.x, v01.x, f.y), InterpBlock(v00.y, v01.y, f.y));
    int2 right = int2(InterpBlock(v10.x, v11.x, f.y), InterpBlock(v10.y, v11.y, f.y));
    return int2(InterpBlock(left.x, right.x, f.x), InterpBlock(left.y, right.y, f.x));
}

uint MaskAt(StructuredBuffer<uint> mask, int2 cell)
{
    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);
    return mask[(uint)cell.y * FlowSize.x + (uint)cell.x];
}

uint InterpolateMask(StructuredBuffer<uint> mask, uint2 pixel)
{
    int2 c0, c1, f;
    GridPosition(pixel, c0, c1, f);
    int a00 = (int)MaskAt(mask, c0);
    int a10 = (int)MaskAt(mask, int2(c1.x, c0.y));
    int a01 = (int)MaskAt(mask, int2(c0.x, c1.y));
    int a11 = (int)MaskAt(mask, c1);
    int left = InterpBlock(a00, a01, f.y);
    int right = InterpBlock(a10, a11, f.y);
    return (uint)clamp(InterpBlock(left, right, f.x), 0, 255);
}

uint4 LoadBytes(Texture2D<float4> frame, int2 p)
{
    p = clamp(p, int2(0, 0), int2(FrameSize) - 1);
    return (uint4)round(saturate(frame.Load(int3(p, 0))) * 255.0);
}

uint4 Blend256(uint4 a, uint4 b, uint weight)
{
    return (a * (256u - weight) + b * weight) >> 8;
}

uint4 Blend255(uint4 a, uint4 b, uint weight)
{
    return (a * (255u - weight) + b * weight + 255u) >> 8;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FrameSize)) return;

    int2 target = int2(id.xy);
    uint algorithm = Control[1];
    uint phase = Control[2];
    uint4 currentA = LoadBytes(PreviousFrame, target);
    uint4 currentB = LoadBytes(NextFrame, target);

    if (algorithm == 0u) {
        OutputFrame[id.xy] = float4(phase < 128u ? currentA : currentB) / 255.0;
        return;
    }

    int2 motionA = InterpolateMotion(FlowBtoA, id.xy, phase);
    int2 motionB = InterpolateMotion(FlowAtoB, id.xy, 256u - phase);
    uint4 warpA = LoadBytes(PreviousFrame, target + motionA);
    uint4 warpB = LoadBytes(NextFrame, target + motionB);

    uint4 result;
    if (algorithm == 21u) {
        uint alphaA = InterpolateMask(MaskA, id.xy);
        uint alphaB = InterpolateMask(MaskB, id.xy);
        uint4 correctedA = Blend255(warpA, warpB, alphaA);
        uint4 correctedB = Blend255(warpB, warpA, alphaB);
        result = Blend256(correctedA, correctedB, phase);
    } else {
        // Recovered algorithm 13 is the per-channel median of the two
        // phase-correct warped endpoint hypotheses and the ordinary temporal
        // sample. clamp(temporal, min, max) is exactly that median.
        uint4 temporal = Blend256(currentA, currentB, phase);
        result = clamp(temporal, min(warpA, warpB), max(warpA, warpB));
    }

    OutputFrame[id.xy] = float4(result) / 255.0;
}
