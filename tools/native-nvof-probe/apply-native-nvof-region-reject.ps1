$ErrorActionPreference = 'Stop'

$py = @'
from pathlib import Path

seed = Path('Source/NvidiaOpticalFlowDenseSeed.hlsl')
seed_text = seed.read_text(encoding='utf-8')
seed_text = seed_text.replace(
    'RWTexture2D<uint> SeedMap : register(u0);\nRWTexture2D<uint> UnsafeCellCount : register(u1);\n',
    'RWTexture2D<uint> SeedMap : register(u0);\nRWTexture2D<uint> UnsafeCellCount : register(u1);\nRWTexture2D<uint> UnsafeCellMap : register(u2);\n',
    1)
old = '''    if (motion > MotionThreshold && consistency > ConsistencyThreshold) {\n        InterlockedAdd(UnsafeCellCount[uint2(0, 0)], 1u);\n    }\n\n    SeedMap[id.xy] = consistency <= ConsistencyThreshold\n        ? PackSeed(id.xy)\n        : InvalidSeed;\n'''
new = '''    bool catastrophic = motion > MotionThreshold && consistency > ConsistencyThreshold;\n    UnsafeCellMap[id.xy] = catastrophic ? 1u : 0u;\n    if (catastrophic) {\n        InterlockedAdd(UnsafeCellCount[uint2(0, 0)], 1u);\n    }\n\n    SeedMap[id.xy] = consistency <= ConsistencyThreshold\n        ? PackSeed(id.xy)\n        : InvalidSeed;\n'''
if old not in seed_text:
    raise RuntimeError('Seed catastrophic block not found')
seed_text = seed_text.replace(old, new, 1)
seed.write_text(seed_text, encoding='utf-8')

region = Path('Source/NvidiaOpticalFlowDenseRegionGate.hlsl')
region.write_text(r'''Texture2D<uint> UnsafeCellMap : register(t0);
RWTexture2D<uint> RegionReject : register(u0);

cbuffer RegionGateParameters : register(b0)
{
    uint2 FlowSize;
    uint MinUnsafeCells;
    uint Radius;
};

uint LoadUnsafe(int2 cell)
{
    if (any(cell < 0) || any(cell >= int2(FlowSize))) return 0u;
    return UnsafeCellMap.Load(int3(cell, 0)) != 0u ? 1u : 0u;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FlowSize)) return;

    int2 center = int2(id.xy);
    if (LoadUnsafe(center) == 0u) return;

    uint unsafeCount = 0u;
    [loop]
    for (int y = -int(Radius); y <= int(Radius); ++y) {
        [loop]
        for (int x = -int(Radius); x <= int(Radius); ++x) {
            unsafeCount += LoadUnsafe(center + int2(x, y));
        }
    }

    // Do not splice real-frame patches into the synthetic image. If a genuine
    // local cluster of catastrophic NVOF cells exists, reject the entire
    // inserted midpoint so spatial coherence is preserved.
    if (unsafeCount >= MinUnsafeCells) {
        InterlockedOr(RegionReject[uint2(0, 0)], 1u);
    }
}
''', encoding='utf-8')

warp = Path('Source/NvidiaOpticalFlowDenseWarp.hlsl')
warp_text = warp.read_text(encoding='utf-8')
warp_text = warp_text.replace(
    'Texture2D<uint> UnsafeCellCount : register(t3);\n',
    'Texture2D<uint> UnsafeCellCount : register(t3);\nTexture2D<uint> RegionReject : register(t4);\n',
    1)
warp_text = warp_text.replace(
    '''    uint unsafeCount = UnsafeCellCount.Load(int3(0, 0, 0));\n    float unsafeFraction = float(unsafeCount) / max(1.0, float(FlowCellCount));\n    if (unsafeFraction >= RepeatBadFraction) {\n''',
    '''    uint unsafeCount = UnsafeCellCount.Load(int3(0, 0, 0));\n    float unsafeFraction = float(unsafeCount) / max(1.0, float(FlowCellCount));\n    bool rejectRegion = RegionReject.Load(int3(0, 0, 0)) != 0u;\n    if (unsafeFraction >= RepeatBadFraction || rejectRegion) {\n''',
    1)
warp.write_text(warp_text, encoding='utf-8')

header = Path('Source/NvidiaOpticalFlowDenseSynthesizer.h')
h = header.read_text(encoding='utf-8')
needle = '''    struct JumpParameters {\n'''
region_struct = '''    struct RegionGateParameters {\n        UINT flowWidth;\n        UINT flowHeight;\n        UINT minUnsafeCells;\n        UINT radius;\n    };\n    static_assert(sizeof(RegionGateParameters) == 16);\n\n'''
if needle not in h:
    raise RuntimeError('Header JumpParameters marker not found')
h = h.replace(needle, region_struct + needle, 1)
h = h.replace('    CComPtr<ID3D11ComputeShader> m_seedShader;\n', '    CComPtr<ID3D11ComputeShader> m_seedShader;\n    CComPtr<ID3D11ComputeShader> m_regionGateShader;\n', 1)
needle = '''    CComPtr<ID3D11Texture2D> m_qualityTexture;\n    CComPtr<ID3D11ShaderResourceView> m_qualityView;\n    CComPtr<ID3D11UnorderedAccessView> m_qualityUav;\n\n'''
replacement = needle + '''    CComPtr<ID3D11Texture2D> m_unsafeCellTexture;\n    CComPtr<ID3D11ShaderResourceView> m_unsafeCellView;\n    CComPtr<ID3D11UnorderedAccessView> m_unsafeCellUav;\n\n    CComPtr<ID3D11Texture2D> m_regionRejectTexture;\n    CComPtr<ID3D11ShaderResourceView> m_regionRejectView;\n    CComPtr<ID3D11UnorderedAccessView> m_regionRejectUav;\n\n'''
if needle not in h:
    raise RuntimeError('Header quality resource marker not found')
h = h.replace(needle, replacement, 1)
h = h.replace('    CComPtr<ID3D11Buffer> m_seedParameters;\n', '    CComPtr<ID3D11Buffer> m_seedParameters;\n    CComPtr<ID3D11Buffer> m_regionGateParameters;\n', 1)
header.write_text(h, encoding='utf-8')

cpp = Path('Source/NvidiaOpticalFlowDenseSynthesizer.cpp')
s = cpp.read_text(encoding='utf-8')
s = s.replace('#include "NvidiaOpticalFlowDenseSeedBytecode.h"\n', '#include "NvidiaOpticalFlowDenseSeedBytecode.h"\n#include "NvidiaOpticalFlowDenseRegionGateBytecode.h"\n', 1)
s = s.replace('const std::array<ID3D11ShaderResourceView*, 4> nullSrvs = {};\n    const std::array<ID3D11UnorderedAccessView*, 2> nullUavs = {};', 'const std::array<ID3D11ShaderResourceView*, 5> nullSrvs = {};\n    const std::array<ID3D11UnorderedAccessView*, 3> nullUavs = {};', 1)
s = s.replace(
'''    if (!CreateShader(device, g_NvofDenseSeedBytecode, sizeof(g_NvofDenseSeedBytecode),\n            m_seedShader, status, L"dense NVOF validation") ||\n        !CreateShader(device, g_NvofDenseJumpBytecode, sizeof(g_NvofDenseJumpBytecode),\n''',
'''    if (!CreateShader(device, g_NvofDenseSeedBytecode, sizeof(g_NvofDenseSeedBytecode),\n            m_seedShader, status, L"dense NVOF validation") ||\n        !CreateShader(device, g_NvofDenseRegionGateBytecode, sizeof(g_NvofDenseRegionGateBytecode),\n            m_regionGateShader, status, L"dense NVOF regional frame gate") ||\n        !CreateShader(device, g_NvofDenseJumpBytecode, sizeof(g_NvofDenseJumpBytecode),\n''', 1)
marker = '''    hr = device->CreateUnorderedAccessView(m_qualityTexture, nullptr, &m_qualityUav);\n    if (FAILED(hr)) {\n        status = std::format(L"CreateUnorderedAccessView(dense NVOF quality counter) failed ({})", HR2Str(hr));\n        Reset();\n        return false;\n    }\n\n'''
insert = marker + '''    hr = device->CreateTexture2D(&seedDesc, nullptr, &m_unsafeCellTexture);\n    if (FAILED(hr)) {\n        status = std::format(L"CreateTexture2D(dense NVOF unsafe-cell map) failed ({})", HR2Str(hr));\n        Reset();\n        return false;\n    }\n    hr = device->CreateShaderResourceView(m_unsafeCellTexture, nullptr, &m_unsafeCellView);\n    if (FAILED(hr)) {\n        status = std::format(L"CreateShaderResourceView(dense NVOF unsafe-cell map) failed ({})", HR2Str(hr));\n        Reset();\n        return false;\n    }\n    hr = device->CreateUnorderedAccessView(m_unsafeCellTexture, nullptr, &m_unsafeCellUav);\n    if (FAILED(hr)) {\n        status = std::format(L"CreateUnorderedAccessView(dense NVOF unsafe-cell map) failed ({})", HR2Str(hr));\n        Reset();\n        return false;\n    }\n\n    hr = device->CreateTexture2D(&qualityDesc, nullptr, &m_regionRejectTexture);\n    if (FAILED(hr)) {\n        status = std::format(L"CreateTexture2D(dense NVOF regional reject flag) failed ({})", HR2Str(hr));\n        Reset();\n        return false;\n    }\n    hr = device->CreateShaderResourceView(m_regionRejectTexture, nullptr, &m_regionRejectView);\n    if (FAILED(hr)) {\n        status = std::format(L"CreateShaderResourceView(dense NVOF regional reject flag) failed ({})", HR2Str(hr));\n        Reset();\n        return false;\n    }\n    hr = device->CreateUnorderedAccessView(m_regionRejectTexture, nullptr, &m_regionRejectUav);\n    if (FAILED(hr)) {\n        status = std::format(L"CreateUnorderedAccessView(dense NVOF regional reject flag) failed ({})", HR2Str(hr));\n        Reset();\n        return false;\n    }\n\n'''
if marker not in s:
    raise RuntimeError('CPP quality UAV marker not found')
s = s.replace(marker, insert, 1)
s = s.replace(
'''    if (!CreateConstantBuffer<SeedParameters>(device, m_seedParameters, status, L"dense NVOF seed params") ||\n        !CreateConstantBuffer<JumpParameters>(device, m_jumpParameters, status, L"dense NVOF jump params") ||\n''',
'''    if (!CreateConstantBuffer<SeedParameters>(device, m_seedParameters, status, L"dense NVOF seed params") ||\n        !CreateConstantBuffer<RegionGateParameters>(device, m_regionGateParameters, status, L"dense NVOF regional-gate params") ||\n        !CreateConstantBuffer<JumpParameters>(device, m_jumpParameters, status, L"dense NVOF jump params") ||\n''', 1)
s = s.replace('    m_seedParameters.Release();\n', '    m_seedParameters.Release();\n    m_regionGateParameters.Release();\n', 1)
s = s.replace('    m_qualityTexture.Release();\n', '    m_qualityTexture.Release();\n    m_regionRejectUav.Release();\n    m_regionRejectView.Release();\n    m_regionRejectTexture.Release();\n    m_unsafeCellUav.Release();\n    m_unsafeCellView.Release();\n    m_unsafeCellTexture.Release();\n', 1)
s = s.replace('    m_seedShader.Release();\n', '    m_regionGateShader.Release();\n    m_seedShader.Release();\n', 1)
old_check = '''            !m_seedShader || !m_jumpShader || !m_denseShader || !m_warpShader || !m_qualityUav || !m_qualityView) {\n'''
new_check = '''            !m_seedShader || !m_regionGateShader || !m_jumpShader || !m_denseShader || !m_warpShader ||\n            !m_qualityUav || !m_qualityView || !m_unsafeCellUav || !m_unsafeCellView ||\n            !m_regionRejectUav || !m_regionRejectView) {\n'''
if old_check not in s:
    raise RuntimeError('CPP resource check not found')
s = s.replace(old_check, new_check, 1)
s = s.replace('    context->ClearUnorderedAccessViewUint(m_qualityUav, zero);\n', '    context->ClearUnorderedAccessViewUint(m_qualityUav, zero);\n    context->ClearUnorderedAccessViewUint(m_regionRejectUav, zero);\n', 1)
s = s.replace(
'''    const std::array<ID3D11UnorderedAccessView*, 2> seedOutputs = {\n        m_seedUavs[0], m_qualityUav,\n    };\n''',
'''    const std::array<ID3D11UnorderedAccessView*, 3> seedOutputs = {\n        m_seedUavs[0], m_qualityUav, m_unsafeCellUav,\n    };\n''', 1)
seed_dispatch = '''    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);\n    UnbindCompute(context);\n\n    UINT jumpStep = 1;\n'''
region_dispatch = '''    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);\n    UnbindCompute(context);\n\n    // A local catastrophic cluster can be visually unacceptable even when it\n    // occupies far less than the global 25% threshold. Reject the entire\n    // inserted midpoint rather than compositing real-frame patches locally.\n    const RegionGateParameters regionValues = {\n        m_flowWidth, m_flowHeight, 8u, 3u,\n    };\n    context->UpdateSubresource(m_regionGateParameters, 0, nullptr, &regionValues, 0, 0);\n    ID3D11Buffer* regionBuffer = m_regionGateParameters;\n    ID3D11ShaderResourceView* regionInput = m_unsafeCellView;\n    ID3D11UnorderedAccessView* regionOutput = m_regionRejectUav;\n    context->CSSetShader(m_regionGateShader, nullptr, 0);\n    context->CSSetConstantBuffers(0, 1, &regionBuffer);\n    context->CSSetShaderResources(0, 1, &regionInput);\n    context->CSSetUnorderedAccessViews(0, 1, &regionOutput, nullptr);\n    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);\n    UnbindCompute(context);\n\n    UINT jumpStep = 1;\n'''
if seed_dispatch not in s:
    raise RuntimeError('CPP seed dispatch marker not found')
s = s.replace(seed_dispatch, region_dispatch, 1)
s = s.replace(
'''    const std::array<ID3D11ShaderResourceView*, 4> warpInputs = {\n        previousFrame, nextFrame, m_denseFlowView, m_qualityView,\n    };\n''',
'''    const std::array<ID3D11ShaderResourceView*, 5> warpInputs = {\n        previousFrame, nextFrame, m_denseFlowView, m_qualityView, m_regionRejectView,\n    };\n''', 1)
cpp.write_text(s, encoding='utf-8')
'@

$pyPath = Join-Path $env:TEMP 'apply-native-nvof-region-reject.py'
[IO.File]::WriteAllText($pyPath, $py)
python $pyPath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Remove-Item $pyPath -Force

$sdkBin = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$fxc = Get-ChildItem $sdkBin -Filter fxc.exe -Recurse -File |
  Sort-Object FullName -Descending |
  Select-Object -First 1
if (-not $fxc) { throw 'fxc.exe was not found in the Windows SDK.' }

$specs = @(
  @{ Source='Source/NvidiaOpticalFlowDenseSeed.hlsl'; Header='Source/NvidiaOpticalFlowDenseSeedBytecode.h'; Name='g_NvofDenseSeedBytecode' },
  @{ Source='Source/NvidiaOpticalFlowDenseRegionGate.hlsl'; Header='Source/NvidiaOpticalFlowDenseRegionGateBytecode.h'; Name='g_NvofDenseRegionGateBytecode' },
  @{ Source='Source/NvidiaOpticalFlowDenseJump.hlsl'; Header='Source/NvidiaOpticalFlowDenseJumpBytecode.h'; Name='g_NvofDenseJumpBytecode' },
  @{ Source='Source/NvidiaOpticalFlowDenseUpsample.hlsl'; Header='Source/NvidiaOpticalFlowDenseUpsampleBytecode.h'; Name='g_NvofDenseUpsampleBytecode' },
  @{ Source='Source/NvidiaOpticalFlowDenseWarp.hlsl'; Header='Source/NvidiaOpticalFlowDenseWarpBytecode.h'; Name='g_NvofDenseWarpBytecode' }
)
foreach ($spec in $specs) {
  & $fxc.FullName /nologo /T cs_5_0 /E main /O3 /WX /Fh $spec.Header /Vn $spec.Name $spec.Source
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add Source/NvidiaOpticalFlowDenseSeed.hlsl Source/NvidiaOpticalFlowDenseSeedBytecode.h `
        Source/NvidiaOpticalFlowDenseRegionGate.hlsl Source/NvidiaOpticalFlowDenseRegionGateBytecode.h `
        Source/NvidiaOpticalFlowDenseWarp.hlsl Source/NvidiaOpticalFlowDenseWarpBytecode.h `
        Source/NvidiaOpticalFlowDenseSynthesizer.cpp Source/NvidiaOpticalFlowDenseSynthesizer.h
if (-not (git diff --cached --quiet)) {
  git commit -m 'Reject midpoints with catastrophic local flow clusters'
  git push origin HEAD:feature/native-nvof-interpolation
}
