$ErrorActionPreference = 'Stop'

$py = @'
from pathlib import Path
import re

native = Path('Source/NvidiaOpticalFlowNative.cpp')
s = native.read_text(encoding='utf-8')

s = s.replace('#include "NvidiaOpticalFlowMidpointBytecode.h"\n', '', 1)

pattern = re.compile(r'\nconstexpr char MidpointShader\[\] = R"hlsl\(.*?static_assert\(sizeof\(ShaderParameters\) == 32\);\n', re.S)
s, count = pattern.subn('\n', s, count=1)
if count != 1:
    raise RuntimeError(f'Expected to remove one legacy midpoint shader block, removed {count}')

for old in (
    '\tCComPtr<ID3D11ComputeShader> midpointShader;\n',
    '\tCComPtr<ID3D11SamplerState> sampler;\n',
    '\tCComPtr<ID3D11Buffer> parameters;\n',
    '\t\tmidpointShader.Release();\n',
    '\t\tsampler.Release();\n',
    '\t\tparameters.Release();\n',
):
    if old not in s:
        raise RuntimeError(f'Legacy midpoint resource marker missing: {old!r}')
    s = s.replace(old, '', 1)

pattern = re.compile(r'''\n\t\thr = device->CreateComputeShader\(\n\t\t\tg_NativeNvofMidpointBytecode,.*?\n\t\tif \(FAILED\(hr\)\) \{\n\t\t\tstatus = std::format\(L"CreateBuffer\(native midpoint params\) failed \(\{\}\)", HR2Str\(hr\)\);\n\t\t\treturn false;\n\t\t\}\n''', re.S)
s, count = pattern.subn('\n', s, count=1)
if count != 1:
    raise RuntimeError(f'Expected to remove one legacy midpoint resource creation block, removed {count}')

native.write_text(s, encoding='utf-8')

proj = Path('Source/MpcVideoRenderer.vcxproj')
p = proj.read_text(encoding='utf-8-sig')
for line in (
    '    <ClCompile Include="NvidiaOpticalFlowSplatSynthesizer.cpp" />\n',
    '    <ClInclude Include="NvidiaOpticalFlowSplatSynthesizer.h" />\n',
):
    if line not in p:
        raise RuntimeError(f'Project cleanup marker missing: {line!r}')
    p = p.replace(line, '', 1)
proj.write_text(p, encoding='utf-8-sig')

workflow = Path('.github/workflows/native-nvof-probe.yml')
w = workflow.read_text(encoding='utf-8')
block = '''            @{\n              Path = 'Source/NvidiaOpticalFlowNative.cpp'\n              Name = 'MidpointShader'\n              Output = 'NativeNvofProductionMidpoint'\n            },\n'''
if block not in w:
    raise RuntimeError('Legacy production midpoint HLSL validation block not found')
w = w.replace(block, '', 1)
workflow.write_text(w, encoding='utf-8')
'@

$py | python -
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$dead = @(
  'Source/NvidiaOpticalFlowMidpointBytecode.h',
  'Source/NvidiaOpticalFlowSplatSynthesizer.cpp',
  'Source/NvidiaOpticalFlowSplatSynthesizer.h',
  'Source/NvidiaOpticalFlowSplatBytecode.h',
  'Source/NvidiaOpticalFlowResolveBytecode.h'
)
foreach ($path in $dead) {
  if (Test-Path $path) { Remove-Item -Force $path }
}

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add -A Source .github/workflows/native-nvof-probe.yml
git commit -m 'Remove abandoned native NVOF synthesis paths'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git push origin HEAD:feature/native-nvof-interpolation
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
