$ErrorActionPreference = 'Stop'

$helper = 'Source/NvidiaOpticalFlowSplatSynthesizer.cpp'
$native = 'Source/NvidiaOpticalFlowNative.cpp'
$project = 'Source/MpcVideoRenderer.vcxproj'

if (Test-Path 'Source/NvidiaOpticalFlowSplatBytecode.h') {
    Write-Host 'Forward-splat bytecode already generated.'
} else {
    $sdkBin = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    $fxc = Get-ChildItem $sdkBin -Filter fxc.exe -Recurse -File |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $fxc) { throw 'fxc.exe was not found in the Windows SDK.' }

    $extract = @'
from pathlib import Path
src = Path('Source/NvidiaOpticalFlowSplatSynthesizer.cpp').read_text(encoding='utf-8')
Path('build').mkdir(exist_ok=True)
for marker, out in [
    ('constexpr char SplatShaderSource[] = R"hlsl(', 'build/NativeNvofSplat.hlsl'),
    ('constexpr char ResolveShaderSource[] = R"hlsl(', 'build/NativeNvofResolve.hlsl'),
]:
    start = src.index(marker) + len(marker)
    end = src.index(')hlsl";', start)
    Path(out).write_text(src[start:end], encoding='utf-8')
'@
    $extract | python -
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $fxc.FullName /nologo /T cs_5_0 /E main /O3 /WX `
        /Fh Source/NvidiaOpticalFlowSplatBytecode.h `
        /Vn g_NativeNvofSplatBytecode build/NativeNvofSplat.hlsl
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $fxc.FullName /nologo /T cs_5_0 /E main /O3 /WX `
        /Fh Source/NvidiaOpticalFlowResolveBytecode.h `
        /Vn g_NativeNvofResolveBytecode build/NativeNvofResolve.hlsl
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$patch = @'
from pathlib import Path
import re

native_path = Path('Source/NvidiaOpticalFlowNative.cpp')
s = native_path.read_text(encoding='utf-8')

if '#include "NvidiaOpticalFlowSplatSynthesizer.h"' not in s:
    s = s.replace('#include "NvidiaOpticalFlowNative.h"\n',
                  '#include "NvidiaOpticalFlowNative.h"\n#include "NvidiaOpticalFlowSplatSynthesizer.h"\n', 1)

member = '\tstd::unique_ptr<CNvidiaOpticalFlowSplatSynthesizer> splatSynthesizer;\n'
anchor = '\tCComPtr<ID3D11Buffer> parameters;\n'
if member not in s:
    if anchor not in s: raise RuntimeError('member anchor not found')
    s = s.replace(anchor, anchor + member, 1)

reset_anchor = '\t\tparameters.Release();\n'
if '\t\tsplatSynthesizer.reset();\n' not in s:
    if reset_anchor not in s: raise RuntimeError('reset anchor not found')
    s = s.replace(reset_anchor, reset_anchor + '\t\tsplatSynthesizer.reset();\n', 1)

create_anchor = '''\t\thr = device->CreateBuffer(&bufferDesc, nullptr, &parameters);\n\t\tif (FAILED(hr)) {\n\t\t\tstatus = std::format(L"CreateBuffer(native midpoint params) failed ({})", HR2Str(hr));\n\t\t\treturn false;\n\t\t}\n\t\treturn true;\n'''
create_repl = '''\t\thr = device->CreateBuffer(&bufferDesc, nullptr, &parameters);\n\t\tif (FAILED(hr)) {\n\t\t\tstatus = std::format(L"CreateBuffer(native midpoint params) failed ({})", HR2Str(hr));\n\t\t\treturn false;\n\t\t}\n\n\t\tsplatSynthesizer = std::make_unique<CNvidiaOpticalFlowSplatSynthesizer>();\n\t\tif (!splatSynthesizer->Initialize(device, width, height, flowWidth, flowHeight, status)) {\n\t\t\treturn false;\n\t\t}\n\t\treturn true;\n'''
if 'splatSynthesizer->Initialize(device, width, height' not in s:
    if create_anchor not in s: raise RuntimeError('CreateSynthesisResources anchor not found')
    s = s.replace(create_anchor, create_repl, 1)

pat = re.compile(r'\n\tvoid DispatchMidpoint\(const float midpointTime\)\n\t\{.*?\n\t\}\n\n\tbool BeginInputFrame', re.S)
replacement = '''
\tbool DispatchMidpoint(const float midpointTime)
\t{
\t\tif (!splatSynthesizer) {
\t\t\tstatus = L"Native NVOF forward-splat synthesizer is unavailable";
\t\t\treturn false;
\t\t}
\t\treturn splatSynthesizer->Dispatch(
\t\t\tcontext,
\t\t\tinputs[currentIndex].view,
\t\t\tinputs[writeIndex].view,
\t\t\tforwardFlow.view,
\t\t\tbackwardFlow.view,
\t\t\toutputUav,
\t\t\tmidpointTime,
\t\t\tstatus);
\t}

\tbool BeginInputFrame'''
s, n = pat.subn(replacement, s, count=1)
if n != 1: raise RuntimeError(f'DispatchMidpoint replacement count={n}')

old_call = '\t\tDispatchMidpoint(midpointTime);\n'
new_call = '''\t\tif (!DispatchMidpoint(midpointTime)) {\n\t\t\tFinish();\n\t\t\treturn false;\n\t\t}\n'''
if old_call in s:
    s = s.replace(old_call, new_call, 1)
elif new_call not in s:
    raise RuntimeError('DispatchMidpoint call not found')

s = s.replace('4x4 bidirectional flow; renderer-owned synthesis; live cost disabled',
              '4x4 bidirectional flow; forward-splat winner synthesis; live cost disabled')

native_path.write_text(s, encoding='utf-8')

proj_path = Path('Source/MpcVideoRenderer.vcxproj')
p = proj_path.read_text(encoding='utf-8')
if 'NvidiaOpticalFlowSplatSynthesizer.cpp' not in p:
    p = p.replace('    <ClCompile Include="NvidiaOpticalFlowNative.cpp" />\n',
                  '    <ClCompile Include="NvidiaOpticalFlowNative.cpp" />\n    <ClCompile Include="NvidiaOpticalFlowSplatSynthesizer.cpp" />\n', 1)
if 'NvidiaOpticalFlowSplatSynthesizer.h' not in p:
    p = p.replace('    <ClInclude Include="NvidiaOpticalFlowNative.h" />\n',
                  '    <ClInclude Include="NvidiaOpticalFlowNative.h" />\n    <ClInclude Include="NvidiaOpticalFlowSplatSynthesizer.h" />\n', 1)
proj_path.write_text(p, encoding='utf-8')
'@
$patch | python -
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add Source/NvidiaOpticalFlowNative.cpp Source/NvidiaOpticalFlowSplatSynthesizer.cpp Source/NvidiaOpticalFlowSplatSynthesizer.h Source/NvidiaOpticalFlowSplatBytecode.h Source/NvidiaOpticalFlowResolveBytecode.h Source/MpcVideoRenderer.vcxproj
git commit -m 'Replace native NVOF inverse warp with forward splat synthesis'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git push origin HEAD:feature/native-nvof-interpolation
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
