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

float MappingTopologyReject(float2 columnX, float2 columnY)
{
    float determinant = columnX.x * columnY.y - columnX.y * columnY.x;
    float frobeniusSq = dot(columnX, columnX) + dot(columnY, columnY);
    float discriminant = sqrt(max(
        frobeniusSq * frobeniusSq - 4.0 * determinant * determinant, 0.0));
    float sigmaMax = sqrt(max(0.5 * (frobeniusSq + discriminant), 0.0));
    float sigmaMin = sqrt(max(0.5 * (frobeniusSq - discriminant), 0.0));

    // A physically plausible midpoint mapping should remain orientation
    // preserving and reasonably well conditioned. Motion-blurred object edges
    // can still pass the photometric guard while the dense flow locally folds
    // or stretches the image into rubbery geometry.
    float foldReject = 1.0 - smoothstep(0.10, 0.35, determinant);
    float stretchReject = smoothstep(2.25, 3.50, sigmaMax);
    float compressionReject = 1.0 - smoothstep(0.20, 0.45, sigmaMin);
    return saturate(max(foldReject, max(stretchReject, compressionReject)));
}

float DenseTopologyReject(float2 pixel, float towardPrevious)
{
    float2 dx = 0.5 * (
        SampleDenseFlow(pixel + float2(1.0, 0.0)) -
        SampleDenseFlow(pixel - float2(1.0, 0.0)));
    float2 dy = 0.5 * (
        SampleDenseFlow(pixel + float2(0.0, 1.0)) -
        SampleDenseFlow(pixel - float2(0.0, 1.0)));

    // Forward map from the B-frame coordinate to the requested midpoint.
    float2 columnX = float2(1.0, 0.0) + towardPrevious * dx;
    float2 columnY = float2(0.0, 1.0) + towardPrevious * dy;
    return MappingTopologyReject(columnX, columnY);
}

float RepairTopologyReject(float2 pixel)
{
    float2 dx = 0.5 * (
        SampleRepair(pixel + float2(1.0, 0.0)).xy -
        SampleRepair(pixel - float2(1.0, 0.0)).xy);
    float2 dy = 0.5 * (
        SampleRepair(pixel + float2(0.0, 1.0)).xy -
        SampleRepair(pixel - float2(0.0, 1.0)).xy);

    // The repair field is A->B motion sampled at midpoint coordinates. Check
    // both endpoint sampling maps because either side can fold independently.
    float2 previousColumnX = float2(1.0, 0.0) - MidpointTime * dx;
    float2 previousColumnY = float2(0.0, 1.0) - MidpointTime * dy;
    float2 nextColumnX = float2(1.0, 0.0) + (1.0 - MidpointTime) * dx;
    float2 nextColumnY = float2(0.0, 1.0) + (1.0 - MidpointTime) * dy;

    return max(
        MappingTopologyReject(previousColumnX, previousColumnY),
        MappingTopologyReject(nextColumnX, nextColumnY));
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
    float denseTopologyReject = DenseTopologyReject(source, towardPrevious);
    float4 current = denseNext;

    // Local occlusion repair remains the preferred alternative when its own
    // symmetrically warped endpoints agree. The topology guard can also invoke
    // this smoother repair field outside the old catastrophic mask when the
    // dense mapping itself is folding or stretching.
    float repairMask = max(SampleRepair(source).z, SampleRepair(target).z);
    float repairPhotoError = densePhotoError;
    float repairTopologyReject = 1.0;
    float repairTrust = 0.0;
    float repairBlend = 0.0;
    float2 repairMotion = 0.0;
    if (repairMask > 1.0e-4 || denseTopologyReject > 1.0e-4) {
        repairMotion = SampleRepair(target).xy;
        float2 previousSource = target - MidpointTime * repairMotion;
        float2 nextSource = target + (1.0 - MidpointTime) * repairMotion;
        float4 safePrevious = SampleFrame(PreviousFrame, previousSource);
        float4 safeNext = SampleFrame(NextFrame, nextSource);
        float4 safe = lerp(safePrevious, safeNext, MidpointTime);
        repairPhotoError = dot(
            abs(safePrevious.rgb - safeNext.rgb), float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
        repairTopologyReject = RepairTopologyReject(target);
        float repairPhotoTrust = 1.0 - smoothstep(0.035, 0.10, repairPhotoError);
        repairTrust = repairPhotoTrust * (1.0 - repairTopologyReject);
        repairBlend = saturate(max(repairMask, denseTopologyReject) * repairTrust);
        current = lerp(current, safe, repairBlend);
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
    float oldLocalFallback = repairMask * max(photoReject, disagreementReject);
    float unresolvedTopology = denseTopologyReject * (1.0 - repairTrust);
    float localFallback = saturate(max(oldLocalFallback, unresolvedTopology));
    if (localFallback > 1.0e-4) {
        float4 temporalMidpoint = lerp(
            SampleFrame(PreviousFrame, target),
            SampleFrame(NextFrame, target),
            MidpointTime);
        current = lerp(current, temporalMidpoint, localFallback);
    }

    OutputFrame[id.xy] = current;
}
