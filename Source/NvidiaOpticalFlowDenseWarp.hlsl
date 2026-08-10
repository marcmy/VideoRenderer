Texture2D<float4> NextFrame : register(t0);
Texture2D<float2> DenseFlow : register(t1);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> OutputFrame : register(u0);

cbuffer WarpParameters : register(b0)
{
    uint2 FrameSize;
    float MidpointTime;
    float Padding;
};

float2 PixelToUv(float2 pixel)
{
    return (pixel + 0.5) / float2(FrameSize);
}

float2 SampleDenseFlow(float2 pixel)
{
    return DenseFlow.SampleLevel(LinearClamp, PixelToUv(pixel), 0.0);
}

float4 SampleNextFrame(float2 pixel)
{
    return NextFrame.SampleLevel(LinearClamp, PixelToUv(pixel), 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FrameSize)) return;

    float2 target = float2(id.xy);
    float2 source = target;
    float towardPrevious = 1.0 - MidpointTime;

    [unroll]
    for (int iteration = 0; iteration < 6; ++iteration) {
        float2 flow = SampleDenseFlow(source);
        source = target - towardPrevious * flow;
    }

    OutputFrame[id.xy] = SampleNextFrame(source);
}
