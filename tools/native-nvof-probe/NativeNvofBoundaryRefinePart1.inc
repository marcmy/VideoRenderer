/*
 * Driver-only NVIDIA Optical Flow motion-edge refinement test.
 *
 * Extends the v6 occlusion-aware probe with edge-aware local motion-vector
 * hypotheses. Unlike the failed wide source-position search, every added
 * candidate is geometrically implied by an actual nearby NVOF vector.
 * The v6 output is produced in the same run and remains the quality baseline.
 */

#define wmain NativeNvofOcclusionAwareReferenceMain
#include "NativeNvofOcclusionAwareTest.cpp"
#undef wmain

namespace {

constexpr char BoundarySearchComputeShader[] = R"hlsl(
cbuffer Parameters : register(b0)
{
    uint2 FrameSize;
    uint2 FlowSize;
    float MidpointTime;
    float GridSize;
    float2 Padding;
};

Texture2D<float4> FirstFrame : register(t0);
Texture2D<float4> SecondFrame : register(t1);
Texture2D<int2> ForwardFlow : register(t2);   // B -> A
Texture2D<int2> BackwardFlow : register(t3);  // A -> B
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> OutputFrame : register(u0);
RWTexture2D<float4> SelectionMap : register(u1);
RWTexture2D<float4> ConfidenceMap : register(u2);

struct Candidate
{
    float2 position;
    float4 color;
    float score;
    float projectionError;
    float consistencyError;
    float photometricError;
    float valid;
};

int2 LoadFlowVector(bool backward, int2 coordinate)
{
    coordinate = clamp(coordinate, int2(0, 0), int2(FlowSize) - 1);
    return backward
        ? BackwardFlow.Load(int3(coordinate, 0))
        : ForwardFlow.Load(int3(coordinate, 0));
}

float2 LoadFlowPixels(bool backward, int2 coordinate)
{
    return float2(LoadFlowVector(backward, coordinate)) / 32.0;
}

float2 SampleFlow(bool backward, float2 pixel)
{
    float2 grid = pixel / GridSize;
    grid = clamp(grid, 0.0, float2(FlowSize - 1));

    int2 p0 = int2(floor(grid));
    int2 p1 = min(p0 + 1, int2(FlowSize - 1));
    float2 fraction = grid - float2(p0);

    float2 f00 = LoadFlowPixels(backward, int2(p0.x, p0.y));
    float2 f10 = LoadFlowPixels(backward, int2(p1.x, p0.y));
    float2 f01 = LoadFlowPixels(backward, int2(p0.x, p1.y));
    float2 f11 = LoadFlowPixels(backward, int2(p1.x, p1.y));

    float2 top = lerp(f00, f10, fraction.x);
    float2 bottom = lerp(f01, f11, fraction.x);
    return lerp(top, bottom, fraction.y);
}

bool IsValid(float2 position)
{
    return all(position >= 0.0) &&
           all(position <= float2(FrameSize - 1));
}

float4 SampleFrame(Texture2D<float4> frame, float2 position)
{
    float2 uv = (position + 0.5) / float2(FrameSize);
    return frame.SampleLevel(LinearClamp, uv, 0.0);
}

float ColorDifference(float4 first, float4 second)
{
    float3 difference = abs(first.rgb - second.rgb);
    return dot(difference, float3(0.299, 0.587, 0.114));
}

Candidate InvalidCandidate()
{
    Candidate candidate;
    candidate.position = 0.0;
    candidate.color = float4(0.0, 0.0, 0.0, 1.0);
    candidate.score = 10000.0;
    candidate.projectionError = 1000.0;
    candidate.consistencyError = 1000.0;
    candidate.photometricError = 1.0;
    candidate.valid = 0.0;
    return candidate;
}

float OppositeConsistency(
    bool oppositeBackward,
    float2 matchingPosition,
    float2 sourceFlow)
{
    float best = length(sourceFlow + SampleFlow(oppositeBackward, matchingPosition));
    int2 center = int2(round(matchingPosition / GridSize));

    // At a flow discontinuity the bilinear opposite field may be a mixture.
    // Compare against nearby native grid vectors and keep the best genuine
    // forward/backward correspondence.
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 opposite = LoadFlowPixels(
                oppositeBackward, center + int2(x, y));
            best = min(best, length(sourceFlow + opposite));
        }
    }
    return best;
}

Candidate EvaluateFirstWithFlow(
    float2 sourcePosition,
    float2 targetPixel,
    float2 sourceToSecond)
{
    Candidate candidate = InvalidCandidate();
    if (!IsValid(sourcePosition)) {
        return candidate;
    }

    float2 projectedMidpoint =
        sourcePosition + MidpointTime * sourceToSecond;
    float2 matchingSecond = sourcePosition + sourceToSecond;

    candidate.position = sourcePosition;
    candidate.color = SampleFrame(FirstFrame, sourcePosition);
    candidate.projectionError = length(projectedMidpoint - targetPixel);
    candidate.consistencyError = 64.0;
    candidate.photometricError = 1.0;
    candidate.valid = 1.0;

    if (IsValid(matchingSecond)) {
        float4 secondColor = SampleFrame(SecondFrame, matchingSecond);
        candidate.consistencyError = OppositeConsistency(
            false, matchingSecond, sourceToSecond);
        candidate.photometricError =
            ColorDifference(candidate.color, secondColor);
    }

    candidate.score =
        4.0 * candidate.projectionError +
        0.30 * candidate.consistencyError +
        10.0 * candidate.photometricError;
    return candidate;
}

Candidate EvaluateSecondWithFlow(
    float2 sourcePosition,
    float2 targetPixel,
    float2 sourceToFirst)
{
    Candidate candidate = InvalidCandidate();
    if (!IsValid(sourcePosition)) {
        return candidate;
    }

    float2 projectedMidpoint =
        sourcePosition + (1.0 - MidpointTime) * sourceToFirst;
    float2 matchingFirst = sourcePosition + sourceToFirst;

    candidate.position = sourcePosition;
    candidate.color = SampleFrame(SecondFrame, sourcePosition);
    candidate.projectionError = length(projectedMidpoint - targetPixel);
    candidate.consistencyError = 64.0;
    candidate.photometricError = 1.0;
    candidate.valid = 1.0;

    if (IsValid(matchingFirst)) {
        float4 firstColor = SampleFrame(FirstFrame, matchingFirst);
        candidate.consistencyError = OppositeConsistency(
            true, matchingFirst, sourceToFirst);
        candidate.photometricError =
            ColorDifference(candidate.color, firstColor);
    }

    candidate.score =
        4.0 * candidate.projectionError +
        0.30 * candidate.consistencyError +
        10.0 * candidate.photometricError;
    return candidate;
}

Candidate EvaluateFirstCandidate(float2 sourcePosition, float2 targetPixel)
{
    return EvaluateFirstWithFlow(
        sourcePosition, targetPixel, SampleFlow(true, sourcePosition));
}

Candidate EvaluateSecondCandidate(float2 sourcePosition, float2 targetPixel)
{
    return EvaluateSecondWithFlow(
        sourcePosition, targetPixel, SampleFlow(false, sourcePosition));
}

Candidate BetterCandidate(Candidate current, Candidate alternative)
{
    Candidate best = current;
    if (alternative.score < current.score) {
        best = alternative;
    }
    return best;
}

Candidate BetterEdgeCandidate(Candidate current, Candidate alternative)
{
    Candidate best = current;
    // Preserve the v6-style solution unless a native-vector hypothesis is
    // meaningfully better. This prevents an edge search from trading a tiny
    // score change for a different motion layer.
    if (alternative.valid > 0.5 &&
        alternative.score + 0.30 < current.score) {
        best = alternative;
    }
    return best;
}

Candidate FindFirstCandidate(float2 targetPixel)
{
    float2 position = targetPixel;
    Candidate best = EvaluateFirstCandidate(position, targetPixel);

    [unroll]
    for (int iteration = 0; iteration < 6; ++iteration) {
        float2 flow = SampleFlow(true, position);
        position = targetPixel - MidpointTime * flow;
        best = BetterCandidate(
            best, EvaluateFirstCandidate(position, targetPixel));
    }

    int2 center = int2(round(targetPixel / GridSize));
    [unroll]
    for (int offsetY = -2; offsetY <= 2; ++offsetY) {
        [unroll]
        for (int offsetX = -2; offsetX <= 2; ++offsetX) {
            float2 flow = LoadFlowPixels(
                true, center + int2(offsetX, offsetY));
            float2 sourcePosition =
                targetPixel - MidpointTime * flow;
            Candidate alternative = EvaluateFirstWithFlow(
                sourcePosition, targetPixel, flow);
            best = BetterEdgeCandidate(best, alternative);
        }
    }
    return best;
}

Candidate FindSecondCandidate(float2 targetPixel)
{
    float2 position = targetPixel;
    Candidate best = EvaluateSecondCandidate(position, targetPixel);

    [unroll]
    for (int iteration = 0; iteration < 6; ++iteration) {
        float2 flow = SampleFlow(false, position);
        position = targetPixel - (1.0 - MidpointTime) * flow;
        best = BetterCandidate(
            best, EvaluateSecondCandidate(position, targetPixel));
    }

    int2 center = int2(round(targetPixel / GridSize));
    [unroll]
    for (int offsetY = -2; offsetY <= 2; ++offsetY) {
        [unroll]
        for (int offsetX = -2; offsetX <= 2; ++offsetX) {
            float2 flow = LoadFlowPixels(
                false, center + int2(offsetX, offsetY));
            float2 sourcePosition =
                targetPixel - (1.0 - MidpointTime) * flow;
            Candidate alternative = EvaluateSecondWithFlow(
                sourcePosition, targetPixel, flow);
            best = BetterEdgeCandidate(best, alternative);
        }
    }
    return best;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= FrameSize)) {
        return;
    }

    float2 pixel = float2(dispatchThreadId.xy);
    Candidate first = FindFirstCandidate(pixel);
    Candidate second = FindSecondCandidate(pixel);

    float4 result = float4(0.0, 0.0, 0.0, 1.0);
    float4 selection = float4(1.0, 0.0, 1.0, 1.0);
    float scoreDifference = abs(first.score - second.score);
    float colorDifference = ColorDifference(first.color, second.color);

    if (first.valid > 0.5 && second.valid > 0.5) {
        bool bothReliable =
            first.score < 1.5 && second.score < 1.5 &&
            colorDifference < 0.04;

        if (bothReliable || scoreDifference < 0.35) {
            float firstWeight = rcp(0.10 + first.score);
            float secondWeight = rcp(0.10 + second.score);
            result = (
                firstWeight * first.color +
                secondWeight * second.color) /
                (firstWeight + secondWeight);
            selection = float4(0.0, 1.0, 0.0, 1.0);
        } else if (first.score < second.score) {
            result = first.color;
            selection = float4(1.0, 0.0, 0.0, 1.0);
        } else {
            result = second.color;
            selection = float4(0.0, 0.35, 1.0, 1.0);
        }
    } else if (first.valid > 0.5) {
        result = first.color;
        selection = float4(1.0, 0.0, 0.0, 1.0);
    } else if (second.valid > 0.5) {
        result = second.color;
        selection = float4(0.0, 0.35, 1.0, 1.0);
    }

    float minimumScore = min(first.score, second.score);
    if (minimumScore > 20.0) {
        selection = lerp(selection, float4(1.0, 0.0, 1.0, 1.0), 0.65);
    }

    OutputFrame[dispatchThreadId.xy] = result;
    SelectionMap[dispatchThreadId.xy] = selection;
    ConfidenceMap[dispatchThreadId.xy] = float4(
        saturate(minimumScore / 20.0),
        saturate(scoreDifference / 8.0),
        saturate(colorDifference * 4.0),
        1.0);
}
)hlsl";

