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

    // Reconstruct both real endpoints implied by the solved dense B->A motion.
    // A large global round-trip-error count is common in fast, motion-blurred
    // footage even when much of this correspondence is still photometrically
    // plausible, so do not use the global bad percentage as a synthesis gate.
    float2 denseMotion = SampleDenseFlow(source);
    float4 denseNext = SampleFrame(NextFrame, source);
    float4 densePrevious = SampleFrame(PreviousFrame, source + denseMotion);
    float densePhotoError = dot(
        abs(densePrevious.rgb - denseNext.rgb), float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
    float4 current = denseNext;

    // Local occlusion repair remains the preferred alternative when its own
    // symmetrically warped endpoints agree. If that repair motion disagrees
    // photometrically, leave the dense result alone rather than forcing a bad
    // repair across the whole catastrophic mask.
    float repairMask = max(SampleRepair(source).z, SampleRepair(target).z);
    float repairPhotoError = densePhotoError;
    float2 repairMotion = 0.0;
    if (repairMask > 1.0e-4) {
        repairMotion = SampleRepair(target).xy;
        float2 previousSource = target - MidpointTime * repairMotion;
        float2 nextSource = target + (1.0 - MidpointTime) * repairMotion;
        float4 safePrevious = SampleFrame(PreviousFrame, previousSource);
        float4 safeNext = SampleFrame(NextFrame, nextSource);
        float4 safe = lerp(safePrevious, safeNext, MidpointTime);
        repairPhotoError = dot(
            abs(safePrevious.rgb - safeNext.rgb), float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
        float repairTrust = 1.0 - smoothstep(0.035, 0.10, repairPhotoError);
        current = lerp(current, safe, saturate(repairMask * repairTrust));
    }

    // If BOTH available motion explanations are locally implausible, quarantine
    // only that region with an unwarped temporal midpoint. This trades a small
    // patch of ghost/blur for melted geometry without collapsing the entire
    // frame back to source cadence. Motion disagreement is used only when there
    // is also some image-domain disagreement, avoiding false rejection of the
    // high-motion but visually coherent correspondences seen in the LOTR set.
    float bestPhotoError = min(densePhotoError, repairPhotoError);
    float photoReject = smoothstep(0.035, 0.10, bestPhotoError);
    float motionDisagreement = length(repairMotion + denseMotion);
    float disagreementReject = smoothstep(16.0, 48.0, motionDisagreement)
        * smoothstep(0.015, 0.05, bestPhotoError);
    float localFallback = saturate(repairMask * max(photoReject, disagreementReject));
    if (localFallback > 1.0e-4) {
        float4 temporalMidpoint = lerp(
            SampleFrame(PreviousFrame, target),
            SampleFrame(NextFrame, target),
            MidpointTime);
        current = lerp(current, temporalMidpoint, localFallback);
    }

    OutputFrame[id.xy] = current;
}
