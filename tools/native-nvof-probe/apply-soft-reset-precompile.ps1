$ErrorActionPreference = 'Stop'

if (Test-Path 'Source/NvidiaOpticalFlowMidpointBytecode.h') {
    Write-Host 'Native NVOF precompiled-shader patch already landed.'
    exit 0
}

$sdkBin = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$fxc = Get-ChildItem $sdkBin -Filter fxc.exe -Recurse -File |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $fxc) {
    throw 'fxc.exe was not found in the Windows SDK.'
}

$extract = @'
from pathlib import Path
path = Path('Source/NvidiaOpticalFlowNative.cpp')
source = path.read_text(encoding='utf-8')
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

$patch = @'
from pathlib import Path
import re

path = Path('Source/NvidiaOpticalFlowNative.cpp')
source = path.read_text(encoding='utf-8')

source = source.replace(
    '#include "NvidiaOpticalFlowNative.h"\n',
    '#include "NvidiaOpticalFlowNative.h"\n#include "NvidiaOpticalFlowMidpointBytecode.h"\n',
    1)

pattern = re.compile(
    r'\n\t\tCComPtr<ID3DBlob> bytecode;.*?\n\t\tD3D11_SAMPLER_DESC samplerDesc = \{\};',
    re.S)
replacement = '''
\t\thr = device->CreateComputeShader(
\t\t\tg_NativeNvofMidpointBytecode,
\t\t\tsizeof(g_NativeNvofMidpointBytecode),
\t\t\tnullptr, &midpointShader);
\t\tif (FAILED(hr)) {
\t\t\tstatus = std::format(L"CreateComputeShader(native midpoint bytecode) failed ({})", HR2Str(hr));
\t\t\treturn false;
\t\t}

\t\tD3D11_SAMPLER_DESC samplerDesc = {};'''
source, count = pattern.subn(replacement, source, count=1)
if count != 1:
    raise RuntimeError(f'Expected one runtime D3DCompile block, replaced {count}.')

marker = '\n\tvoid ResetUnlocked()\n\t{'
if marker not in source:
    raise RuntimeError('ResetUnlocked marker not found.')
soft = '''
\tvoid SoftResetUnlocked()
\t{
\t\twriteIndex = currentIndex = 0;
\t\twarmedUp = false;
\t\toutputValid = false;
\t\thasExecutedFlow = false;
\t\thavePreviousTimestamp = false;
\t\tpreviousTimestamp = 0.0;
\t\tprocessTimeMs = 0.0;
\t\tstatus = session ? L"Native NVOF reset; waiting for frames" : L"Disabled";
\t}
'''
source = source.replace(marker, '\n' + soft + marker, 1)

old_dtor = '''CNvidiaOpticalFlowNative::~CNvidiaOpticalFlowNative()
{
\tReset();
}'''
new_dtor = '''CNvidiaOpticalFlowNative::~CNvidiaOpticalFlowNative()
{
#ifdef _WIN64
\tif (m_impl->inputTransaction.owns_lock()) {
\t\tm_impl->inputTransaction.unlock();
\t}
\tstd::lock_guard<std::recursive_mutex> lock(m_impl->apiMutex);
\tm_impl->ResetUnlocked();
#endif
}'''
if old_dtor not in source:
    raise RuntimeError('Destructor patch target not found.')
source = source.replace(old_dtor, new_dtor, 1)

old_reset = '''void CNvidiaOpticalFlowNative::Reset()
{
#ifdef _WIN64
\tif (m_impl->inputTransaction.owns_lock()) {
\t\tm_impl->inputTransaction.unlock();
\t}
\tstd::lock_guard<std::recursive_mutex> lock(m_impl->apiMutex);
\tm_impl->ResetUnlocked();
\tm_impl->status = L"Disabled";
#endif
}'''
new_reset = '''void CNvidiaOpticalFlowNative::Reset()
{
#ifdef _WIN64
\tif (m_impl->inputTransaction.owns_lock()) {
\t\tm_impl->inputTransaction.unlock();
\t}
\tstd::lock_guard<std::recursive_mutex> lock(m_impl->apiMutex);
\t// Playback discontinuities only invalidate frame/temporal history.
\t// Preserve the initialized driver session and D3D11 resources so a seek
\t// cannot force synchronous shader compilation/session reconstruction.
\tm_impl->SoftResetUnlocked();
#endif
}'''
if old_reset not in source:
    raise RuntimeError('Public Reset patch target not found.')
source = source.replace(old_reset, new_reset, 1)

path.write_text(source, encoding='utf-8')
'@
$patch | python -
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add Source/NvidiaOpticalFlowNative.cpp Source/NvidiaOpticalFlowMidpointBytecode.h
git commit -m 'Eliminate native NVOF playback reset stalls'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git push origin HEAD:feature/native-nvof-interpolation
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
