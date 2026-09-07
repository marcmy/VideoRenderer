StructuredBuffer<uint> Counters : register(t0);
RWStructuredBuffer<uint> Control : register(u0);

cbuffer ControlParameters : register(b0)
{
    uint2 FlowSize;
    uint BorderX;
    uint BorderY;
};

// Control layout:
//   0 scene class (0..3)
//   1 effective algorithm (0=cut/fallback, 13)
//   2 phase (stock exact-2x adaptive: C0=128, C1=128, C2=64)
//   3 considered cells
//   4 zero-skipped cells
//   5 m1 bucket count
//   6 m2 bucket count
//   7 scene bucket count
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint fullCount = FlowSize.x * FlowSize.y;
    uint interiorWidth = FlowSize.x > BorderX * 2u ? FlowSize.x - BorderX * 2u : 0u;
    uint interiorHeight = FlowSize.y > BorderY * 2u ? FlowSize.y - BorderY * 2u : 0u;
    uint interiorCount = interiorWidth * interiorHeight;
    uint zeroSkipped = min(Counters[0], (2u * fullCount) / 3u);
    uint considered = interiorCount > zeroSkipped ? interiorCount - zeroSkipped : 0u;

    uint required = (20u * considered) / 100u;
    uint m1 = Counters[1];
    uint m2 = Counters[2];
    uint scene = Counters[3];
    uint high = scene + m2;
    uint mid = high + m1;

    uint sceneClass = 0u;
    if (scene >= required) sceneClass = 3u;
    else if (high >= required) sceneClass = 2u;
    else if (mid >= required) sceneClass = 1u;

    // The active SVP profile requests fi_shader=13 with adaptive=210. Class 3
    // remains on the separate conservative cut/fallback path.
    uint algorithm = sceneClass >= 3u ? 0u : 13u;
    uint phase = sceneClass == 2u ? 64u : 128u;

    Control[0] = sceneClass;
    Control[1] = algorithm;
    Control[2] = phase;
    Control[3] = considered;
    Control[4] = zeroSkipped;
    Control[5] = m1;
    Control[6] = m2;
    Control[7] = scene;
}
