$ErrorActionPreference = 'Stop'

$sourcePath = 'Source/NvidiaOpticalFlowNative.cpp'
$source = [IO.File]::ReadAllText($sourcePath)

if ($source.Contains('Exact offline cell-risk fallback')) {
    Write-Host 'Exact cell-risk fallback already integrated.'
    exit 0
}

$patch = @'
from pathlib import Path
import re

path = Path('Source/NvidiaOpticalFlowNative.cpp')
source = path.read_text(encoding='utf-8')

source = source.replace('#include "NvidiaOpticalFlowSplatSynthesizer.h"\n', '', 1)
source = source.replace('\tstd::unique_ptr<CNvidiaOpticalFlowSplatSynthesizer> splatSynthesizer;\n', '', 1)
source = source.replace('\t\tsplatSynthesizer.reset();\n', '', 1)

create_block = '''\n\t\tsplatSynthesizer = std::make_unique<CNvidiaOpticalFlowSplatSynthesizer>();\n\t\tif (!splatSynthesizer->Initialize(device, width, height, flowWidth, flowHeight, status)) {\n\t\t\treturn false;\n\t\t}\n'''
if create_block not in source:
    raise RuntimeError('Forward-splat initialization block was not found.')
source = source.replace(create_block, '\n', 1)

source = source.replace(
    'L"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow; forward-splat winner synthesis; live cost disabled",',
    'L"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow; occlusion-aware inverse warp + exact cell-risk fallback; live cost disabled",',
    1)

old_dispatch = re.compile(r'''\n\tbool DispatchMidpoint\(const float midpointTime\)\n\t\{\n\t\tif \(!splatSynthesizer\) \{.*?\n\t\}\n\n\tbool BeginInputFrame''', re.S)
new_dispatch = '''
\tbool DispatchMidpoint(const float midpointTime)
\t{
\t\tconst ShaderParameters values = {
\t\t\twidth, height, flowWidth, flowHeight,
\t\t\tmidpointTime, static_cast<float>(FlowGridSize), {0.0f, 0.0f},
\t\t};
\t\tcontext->UpdateSubresource(parameters, 0, nullptr, &values, 0, 0);

\t\tconst std::array<ID3D11ShaderResourceView*, 4> inputsViews = {
\t\t\tinputs[currentIndex].view,
\t\t\tinputs[writeIndex].view,
\t\t\tforwardFlow.view,
\t\t\tbackwardFlow.view,
\t\t};
\t\tID3D11UnorderedAccessView* output = outputUav;
\t\tID3D11Buffer* constantBuffer = parameters;
\t\tID3D11SamplerState* samplerState = sampler;
\t\tcontext->CSSetShader(midpointShader, nullptr, 0);
\t\tcontext->CSSetConstantBuffers(0, 1, &constantBuffer);
\t\tcontext->CSSetSamplers(0, 1, &samplerState);
\t\tcontext->CSSetShaderResources(0, static_cast<UINT>(inputsViews.size()), inputsViews.data());
\t\tcontext->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
\t\tcontext->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

\t\tconst std::array<ID3D11ShaderResourceView*, 4> nullViews = {};
\t\tID3D11UnorderedAccessView* nullOutput = nullptr;
\t\tID3D11Buffer* nullBuffer = nullptr;
\t\tID3D11SamplerState* nullSampler = nullptr;
\t\tcontext->CSSetUnorderedAccessViews(0, 1, &nullOutput, nullptr);
\t\tcontext->CSSetShaderResources(0, static_cast<UINT>(nullViews.size()), nullViews.data());
\t\tcontext->CSSetConstantBuffers(0, 1, &nullBuffer);
\t\tcontext->CSSetSamplers(0, 1, &nullSampler);
\t\tcontext->CSSetShader(nullptr, nullptr, 0);
\t\treturn true;
\t}

\tbool BeginInputFrame'''
source, count = old_dispatch.subn(new_dispatch, source, count=1)
if count != 1:
    raise RuntimeError(f'Expected one forward-splat DispatchMidpoint, replaced {count}.')

helper_marker = '[numthreads(8, 8, 1)]\nvoid main(uint3 id : SV_DispatchThreadID)'
if helper_marker not in source:
    raise RuntimeError('Midpoint shader main marker was not found.')
helpers = r'''
// Exact offline cell-risk fallback. The real-frame replay that removed the
// fast Apex weapon deformation used nearest 4x4-cell flow/consistency values,
// then a 5x5 pixel dilation. Keep this metric separate from SampleFlow(), which
// deliberately modifies vectors for synthesis.
float FlowCellConsistency(bool backward, int2 cell)
{
    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);
    float2 flow = float2(LoadFlowVector(backward, cell)) / 32.0;
    float2 cellPixel = float2(cell) * GridSize;
    float2 reverseFlow = SampleRawFlow(!backward, cellPixel + flow);
    return length(flow + reverseFlow);
}

bool UnsafeFlowCellAtPixel(float2 samplePixel)
{
    samplePixel = clamp(samplePixel, 0.0, float2(FrameSize - 1));
    int2 cell = int2(samplePixel / GridSize);
    float2 firstToSecond = float2(LoadFlowVector(true, cell)) / 32.0;
    float2 secondToFirst = float2(LoadFlowVector(false, cell)) / 32.0;
    float motion = max(length(firstToSecond), length(secondToFirst));
    if (motion <= 20.0) return false;

    float risk = max(
        FlowCellConsistency(true, cell),
        FlowCellConsistency(false, cell));
    return risk > 20.0;
}

bool UnsafeFlowNeighborhood(float2 pixel)
{
    // Because UnsafeFlowCellAtPixel is constant inside each 4x4 NVOF cell,
    // these nine probes reproduce a 5x5 full-resolution dilation exactly.
    [unroll]
    for (int oy = -2; oy <= 2; oy += 2) {
        [unroll]
        for (int ox = -2; ox <= 2; ox += 2) {
            if (UnsafeFlowCellAtPixel(pixel + float2(ox, oy))) return true;
        }
    }
    return false;
}

'''
source = source.replace(helper_marker, helpers + helper_marker, 1)

fallback = re.compile(r'''\n    // Hard safety fallback for fast motion\..*?\n    if \(localMotion >= 20\.0 && max\(firstRoundTripError, secondRoundTripError\) >= 20\.0\) \{\n        result = MidpointTime <= 0\.5\n            \? SampleFrame\(FirstFrame, pixel\)\n            : SampleFrame\(SecondFrame, pixel\);\n    \}\n''', re.S)
replacement = '''
    if (UnsafeFlowNeighborhood(pixel)) {
        result = MidpointTime <= 0.5
            ? SampleFrame(FirstFrame, pixel)
            : SampleFrame(SecondFrame, pixel);
    }
'''
source, count = fallback.subn(replacement, source, count=1)
if count != 1:
    raise RuntimeError(f'Expected one old hard-fallback block, replaced {count}.')

path.write_text(source, encoding='utf-8')
'@
$patch | python -
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$sdkBin = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$fxc = Get-ChildItem $sdkBin -Filter fxc.exe -Recurse -File |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $fxc) { throw 'fxc.exe was not found in the Windows SDK.' }

$extract = @'
from pathlib import Path
source = Path('Source/NvidiaOpticalFlowNative.cpp').read_text(encoding='utf-8')
marker0 = 'constexpr char MidpointShader[] = R"hlsl('
marker1 = ')hlsl";'
start = source.index(marker0) + len(marker0)
end = source.index(marker1, start)
Path('build').mkdir(exist_ok=True)
Path('build/NativeNvofMidpoint.hlsl').write_text(source[start:end], encoding='utf-8')
'@
$extract | python -
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $fxc.FullName /nologo /T cs_5_0 /E main /O3 /WX `
    /Fh Source/NvidiaOpticalFlowMidpointBytecode.h `
    /Vn g_NativeNvofMidpointBytecode `
    build/NativeNvofMidpoint.hlsl
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add Source/NvidiaOpticalFlowNative.cpp Source/NvidiaOpticalFlowMidpointBytecode.h
git commit -m 'Match real-frame NVOF cell-risk fallback'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git push origin HEAD:feature/native-nvof-interpolation
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
