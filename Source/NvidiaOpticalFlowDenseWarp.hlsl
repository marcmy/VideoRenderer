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

    uint unsafeCount = UnsafeCellCount.Load(int3(0, 0, 0));
    float unsafeFraction = float(unsafeCount) / max(1.0, float(FlowCellCount));

    if (unsafeFraction >= RepeatBadFraction) {
        OutputFrame[id.xy] = SampleFrame(PreviousFrame, float2(id.xy));
        return;
    }

    float2 target = float2(id.xy);
    float2 source = target;
    float towardPrevious = 1.0 - MidpointTime;

    [unroll]
    for (int iteration = 0; iteration < 6; ++iteration) {
        float2 flow = SampleDenseFlow(source);
        source = target - towardPrevious * flow;
    }

    float4 current = SampleFrame(NextFrame, source);

    // Local occlusion repair. The mask follows the source coordinate selected
    // by the normal dense B-side warp, while the coherent A->B repair motion
    // is evaluated in target coordinates. Symmetrically motion-compensating
    // both real endpoints avoids the hard patch seams and severe crossfade
    // blur of earlier local fallback experiments.
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

    OutputFrame[id.xy] = current;
}
