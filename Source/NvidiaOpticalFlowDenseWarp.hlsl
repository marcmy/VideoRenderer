Texture2D<float4> PreviousFrame : register(t0);
Texture2D<float4> NextFrame : register(t1);
Texture2D<float2> DenseFlow : register(t2);
Texture2D<uint> UnsafeCellCount : register(t3);
Texture2D<float4> RepairField : register(t4);

SamplerState LinearClamp : register(s0);
RWTexture2D<float4> OutputFrame : register(u0);

cbuffer WarpParameters : register(b0)
{
    uint2 FrameSize;
    uint FlowCellCount;
    float RepeatBadFraction;
    float MidpointTime;
    float RepairGridSize;
    float2 Padding;
};

static const uint SceneCutBit = 0x80000000u;
static const uint UnsafeCountMask = 0x7fffffffu;

float2 PixelToUv(float2 pixel)
{
    return (pixel + 0.5) / float2(FrameSize);
}

float2 SampleDenseFlow(float2 pixel)
{
    return DenseFlow.SampleLevel(LinearClamp, PixelToUv(pixel), 0.0);
}

float4 SampleRepair(float2 pixel)
{
    uint repairWidth, repairHeight;
    RepairField.GetDimensions(repairWidth, repairHeight);
    float2 repairGrid = pixel / max(RepairGridSize, 1.0e-6);
    float2 repairUv = (repairGrid + 0.5) / float2(repairWidth, repairHeight);
    return RepairField.SampleLevel(LinearClamp, repairUv, 0.0);
}

float4 SampleFrame(Texture2D<float4> frame, float2 pixel)
{
    return frame.SampleLevel(LinearClamp, PixelToUv(pixel), 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FrameSize)) return;

    uint packedQuality = UnsafeCellCount.Load(int3(0, 0, 0));
    bool sceneCut = (packedQuality & SceneCutBit) != 0u;
    uint unsafeCount = packedQuality & UnsafeCountMask;
    float unsafeFraction = float(unsafeCount) / max(1.0, float(FlowCellCount));
    float2 target = float2(id.xy);

    // Optical flow has no meaningful solution across an actual hard cut.
    // Preserve source timing by holding the previous shot until the real B
    // frame's timestamp rather than morphing unrelated camera angles.
    if (sceneCut) {
        OutputFrame[id.xy] = SampleFrame(PreviousFrame, target);
        return;
    }

    float2 source = target;
    float towardPrevious = 1.0 - MidpointTime;

    [unroll]
    for (int iteration = 0; iteration < 6; ++iteration) {
        float2 flow = SampleDenseFlow(source);
        source = target - towardPrevious * flow;
    }

    float4 current = SampleFrame(NextFrame, source);

    // Local occlusion repair. The mask follows both the source coordinate
    // selected by the dense B-side warp and the target coordinate. The coherent
    // A->B repair motion is evaluated in target coordinates.
    float repairMask = max(SampleRepair(source).z, SampleRepair(target).z);
    if (repairMask > 1.0e-4) {
        float2 repairMotion = SampleRepair(target).xy;
        float2 previousSource = target - MidpointTime * repairMotion;
        float2 nextSource = target + (1.0 - MidpointTime) * repairMotion;
        float4 safePrevious = SampleFrame(PreviousFrame, previousSource);
        float4 safeNext = SampleFrame(NextFrame, nextSource);
        float4 safe = lerp(safePrevious, safeNext, MidpointTime);
        current = lerp(current, safe, saturate(repairMask));
    }

    // When the flow field collapses over a large fraction of a *same shot*,
    // progressively prefer an unwarped temporal midpoint over melted geometry.
    // Unlike the old whole-frame repeat gate, this keeps a distinct midpoint
    // and therefore preserves the doubled presentation cadence.
    float safetyBlend = smoothstep(0.25, 0.40, unsafeFraction);
    if (safetyBlend > 1.0e-4) {
        float4 temporalMidpoint = lerp(
            SampleFrame(PreviousFrame, target),
            SampleFrame(NextFrame, target),
            MidpointTime);
        current = lerp(current, temporalMidpoint, safetyBlend);
    }

    OutputFrame[id.xy] = current;
}
