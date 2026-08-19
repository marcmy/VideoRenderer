from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected one match, found {count}')
    path.write_text(text.replace(old, new, 1), encoding='utf-8', newline='\n')


seed = ROOT / 'Source/NvidiaOpticalFlowDenseSeed.hlsl'
replace_once(
    seed,
    '''uint PackSeed(uint2 cell)\n{\n    return (cell.y << 16) | (cell.x & 0xffffu);\n}\n''',
    '''static const uint BackwardSeedBit = 0x80000000u;\n\nuint PackSeed(uint2 cell, bool useBackwardSeed)\n{\n    uint packed = (cell.y << 16) | (cell.x & 0xffffu);\n    return useBackwardSeed ? (packed | BackwardSeedBit) : packed;\n}\n''',
    'seed packing')
replace_once(
    seed,
    '''    SeedMap[id.xy] = consistency <= ConsistencyThreshold\n        ? PackSeed(id.xy)\n        : InvalidSeed;\n''',
    '''    // Preserve every trustworthy native B->A seed. If that direction fails\n    // its own round-trip check but A->B is still trustworthy, use the negated\n    // A->B vector as an asymmetric backfill seed instead of creating a large\n    // JFA hole. When both pass, prefer native B->A to preserve existing behavior.\n    bool forwardSeedValid = bToAError <= ConsistencyThreshold;\n    bool backwardSeedValid = aToBError <= ConsistencyThreshold;\n    bool useBackwardSeed = !forwardSeedValid && backwardSeedValid;\n    SeedMap[id.xy] = (forwardSeedValid || backwardSeedValid)\n        ? PackSeed(id.xy, useBackwardSeed)\n        : InvalidSeed;\n''',
    'seed validity')

jump = ROOT / 'Source/NvidiaOpticalFlowDenseJump.hlsl'
replace_once(
    jump,
    '''uint2 UnpackSeed(uint packed)\n{\n    return uint2(packed & 0xffffu, packed >> 16);\n}\n''',
    '''uint2 UnpackSeed(uint packed)\n{\n    // Bit 31 marks an A->B-derived backfill seed; it is not part of Y.\n    return uint2(packed & 0xffffu, (packed >> 16) & 0x7fffu);\n}\n''',
    'jump seed unpack')

upsample = ROOT / 'Source/NvidiaOpticalFlowDenseUpsample.hlsl'
replace_once(
    upsample,
    '''Texture2D<float4> NextFrame : register(t0);\nTexture2D<int2> ForwardFlowBtoA : register(t1);\nTexture2D<uint> SeedMap : register(t2);\n''',
    '''Texture2D<float4> NextFrame : register(t0);\nTexture2D<int2> ForwardFlowBtoA : register(t1);\nTexture2D<int2> BackwardFlowAtoB : register(t2);\nTexture2D<uint> SeedMap : register(t3);\n''',
    'dense inputs')
replace_once(
    upsample,
    '''static const uint InvalidSeed = 0xffffffffu;\n\nuint2 UnpackSeed(uint packed)\n{\n    return uint2(packed & 0xffffu, packed >> 16);\n}\n\nfloat2 LoadRawFlow(int2 cell)\n{\n    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);\n    return float2(ForwardFlowBtoA.Load(int3(cell, 0))) / 32.0;\n}\n''',
    '''static const uint InvalidSeed = 0xffffffffu;\nstatic const uint BackwardSeedBit = 0x80000000u;\n\nuint2 UnpackSeed(uint packed)\n{\n    return uint2(packed & 0xffffu, (packed >> 16) & 0x7fffu);\n}\n\nbool SeedUsesBackward(uint packed)\n{\n    return (packed & BackwardSeedBit) != 0u;\n}\n\nfloat2 LoadRawFlow(int2 cell)\n{\n    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);\n    return float2(ForwardFlowBtoA.Load(int3(cell, 0))) / 32.0;\n}\n\nfloat2 LoadBackwardFlow(int2 cell)\n{\n    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);\n    return float2(BackwardFlowAtoB.Load(int3(cell, 0))) / 32.0;\n}\n\nfloat2 LoadSeedFlow(uint packedSeed)\n{\n    int2 seed = int2(UnpackSeed(packedSeed));\n    return SeedUsesBackward(packedSeed)\n        ? -LoadBackwardFlow(seed)\n        : LoadRawFlow(seed);\n}\n''',
    'dense seed helpers')
replace_once(
    upsample,
    '''            flowSum += LoadRawFlow(int2(seed)) * weight;\n''',
    '''            flowSum += LoadSeedFlow(packedSeed) * weight;\n''',
    'dense seed flow')

cpp = ROOT / 'Source/NvidiaOpticalFlowDenseSynthesizer.cpp'
replace_once(
    cpp,
    '''    const std::array<ID3D11ShaderResourceView*, 3> denseInputs = {\n        nextFrame, forwardFlowBtoA, m_seedViews[seedRead],\n    };\n''',
    '''    const std::array<ID3D11ShaderResourceView*, 4> denseInputs = {\n        nextFrame, forwardFlowBtoA, backwardFlowAtoB, m_seedViews[seedRead],\n    };\n''',
    'dense SRV bindings')
replace_once(
    cpp,
    'L"cut={}, bad {:.1f}% ({}/{}), guard=local-photo, worst7x7 {}/49, would8={}, would18={}",',
    'L"cut={}, bad {:.1f}% ({}/{}), guard=local-photo, seeds=asym-backfill, worst7x7 {}/49, would8={}, would18={}",',
    'telemetry label')

print('Patched asymmetric bidirectional seed backfill.')
