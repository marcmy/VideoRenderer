from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

warp_path = ROOT / 'Source/NvidiaOpticalFlowDenseWarp.hlsl'
warp = warp_path.read_text(encoding='utf-8')

old_quality = '''    uint packedQuality = UnsafeCellCount.Load(int3(0, 0, 0));
    bool sceneCut = (packedQuality & SceneCutBit) != 0u;
    uint unsafeCount = packedQuality & UnsafeCountMask;
    float unsafeFraction = float(unsafeCount) / max(1.0, float(FlowCellCount));
    float2 target = float2(id.xy);
'''
new_quality = '''    uint packedQuality = UnsafeCellCount.Load(int3(0, 0, 0));
    bool sceneCut = (packedQuality & SceneCutBit) != 0u;
    float2 target = float2(id.xy);
'''
if old_quality not in warp:
    raise RuntimeError('Expected global-quality preamble not found in warp shader')
warp = warp.replace(old_quality, new_quality, 1)

old_body = '''    float4 current = SampleFrame(NextFrame, source);

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
'''
new_body = '''    // Reconstruct both real endpoints implied by the solved dense B->A motion.
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
'''
if old_body not in warp:
    raise RuntimeError('Expected global safety-blend body not found in warp shader')
warp = warp.replace(old_body, new_body, 1)
warp = warp.replace('static const uint UnsafeCountMask = 0x7fffffffu;\n', '', 1)
warp_path.write_text(warp, encoding='utf-8', newline='\n')

cpp_path = ROOT / 'Source/NvidiaOpticalFlowDenseSynthesizer.cpp'
cpp = cpp_path.read_text(encoding='utf-8')
old_telemetry = '''    const UINT cellCount = m_flowWidth * m_flowHeight;
    const double unsafeFraction = static_cast<double>(m_lastUnsafeCount) /
        std::max(1u, cellCount);
    const double badPercent = 100.0 * unsafeFraction;
    const double blendT = std::clamp((unsafeFraction - 0.25) / 0.15, 0.0, 1.0);
    const double safetyBlendPercent = 100.0 * blendT * blendT * (3.0 - 2.0 * blendT);
    return std::format(
        L"cut={}, bad {:.1f}% ({}/{}), safetyBlend {:.0f}%, worst7x7 {}/49, would8={}, would18={}",
        m_lastSceneCut ? L"yes" : L"no",
        badPercent, m_lastUnsafeCount, cellCount, safetyBlendPercent, m_lastMaxLocalUnsafe,
        m_lastMaxLocalUnsafe >= 8 ? L"yes" : L"no",
        m_lastMaxLocalUnsafe >= 18 ? L"yes" : L"no");
'''
new_telemetry = '''    const UINT cellCount = m_flowWidth * m_flowHeight;
    const double badPercent = 100.0 * static_cast<double>(m_lastUnsafeCount) /
        std::max(1u, cellCount);
    return std::format(
        L"cut={}, bad {:.1f}% ({}/{}), guard=local-photo, worst7x7 {}/49, would8={}, would18={}",
        m_lastSceneCut ? L"yes" : L"no",
        badPercent, m_lastUnsafeCount, cellCount, m_lastMaxLocalUnsafe,
        m_lastMaxLocalUnsafe >= 8 ? L"yes" : L"no",
        m_lastMaxLocalUnsafe >= 18 ? L"yes" : L"no");
'''
if old_telemetry not in cpp:
    raise RuntimeError('Expected global safety-blend telemetry not found')
cpp = cpp.replace(old_telemetry, new_telemetry, 1)
cpp_path.write_text(cpp, encoding='utf-8', newline='\n')

print('Patched local photometric NVOF guard.')
