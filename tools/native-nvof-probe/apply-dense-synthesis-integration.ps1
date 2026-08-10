$ErrorActionPreference = 'Stop'

$branch = 'feature/native-nvof-interpolation'

$sdkBin = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$fxc = Get-ChildItem $sdkBin -Filter fxc.exe -Recurse -File |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $fxc) { throw 'fxc.exe was not found in the Windows SDK.' }

$shaderSpecs = @(
    @{ Source='Source/NvidiaOpticalFlowDenseSeed.hlsl'; Header='Source/NvidiaOpticalFlowDenseSeedBytecode.h'; Name='g_NvofDenseSeedBytecode' },
    @{ Source='Source/NvidiaOpticalFlowDenseJump.hlsl'; Header='Source/NvidiaOpticalFlowDenseJumpBytecode.h'; Name='g_NvofDenseJumpBytecode' },
    @{ Source='Source/NvidiaOpticalFlowDenseUpsample.hlsl'; Header='Source/NvidiaOpticalFlowDenseUpsampleBytecode.h'; Name='g_NvofDenseUpsampleBytecode' },
    @{ Source='Source/NvidiaOpticalFlowDenseWarp.hlsl'; Header='Source/NvidiaOpticalFlowDenseWarpBytecode.h'; Name='g_NvofDenseWarpBytecode' }
)
foreach ($spec in $shaderSpecs) {
    & $fxc.FullName /nologo /T cs_5_0 /E main /O3 /WX /Fh $spec.Header /Vn $spec.Name $spec.Source
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$nativePath = 'Source/NvidiaOpticalFlowNative.cpp'
$source = [IO.File]::ReadAllText($nativePath)
if (-not $source.Contains('#include "NvidiaOpticalFlowDenseSynthesizer.h"')) {
    $source = $source.Replace(
        '#include "NvidiaOpticalFlowNative.h"',
        "#include `"NvidiaOpticalFlowNative.h`"`r`n#include `"NvidiaOpticalFlowDenseSynthesizer.h`"")
}

if (-not $source.Contains('std::unique_ptr<CNvidiaOpticalFlowDenseSynthesizer> denseSynthesizer;')) {
    $source = $source.Replace(
        "`tCComPtr<ID3D11Buffer> parameters;",
        "`tCComPtr<ID3D11Buffer> parameters;`r`n`tstd::unique_ptr<CNvidiaOpticalFlowDenseSynthesizer> denseSynthesizer;")
}

if (-not $source.Contains('denseSynthesizer.reset();')) {
    $source = $source.Replace(
        "`t`tparameters.Release();`r`n`t`tmultithread.Release();",
        "`t`tparameters.Release();`r`n`t`tif (denseSynthesizer) {`r`n`t`t`tdenseSynthesizer->Reset();`r`n`t`t`tdenseSynthesizer.reset();`r`n`t`t}`r`n`t`tmultithread.Release();")
}

$createNeedle = "`t`tif (FAILED(hr)) {`r`n`t`t`tstatus = std::format(L`"CreateBuffer(native midpoint params) failed ({})`", HR2Str(hr));`r`n`t`t`treturn false;`r`n`t`t}`r`n`r`n`t`treturn true;"
if (-not $source.Contains('denseSynthesizer = std::make_unique<CNvidiaOpticalFlowDenseSynthesizer>();')) {
    if (-not $source.Contains($createNeedle)) { throw 'Could not locate CreateSynthesisResources tail.' }
    $createReplacement = "`t`tif (FAILED(hr)) {`r`n`t`t`tstatus = std::format(L`"CreateBuffer(native midpoint params) failed ({})`", HR2Str(hr));`r`n`t`t`treturn false;`r`n`t`t}`r`n`r`n`t`tdenseSynthesizer = std::make_unique<CNvidiaOpticalFlowDenseSynthesizer>();`r`n`t`tif (!denseSynthesizer->Initialize(device, width, height, flowWidth, flowHeight, status)) {`r`n`t`t`treturn false;`r`n`t`t}`r`n`r`n`t`treturn true;"
    $source = $source.Replace($createNeedle, $createReplacement)
}

$source = $source.Replace(
    'L"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow; occlusion-aware inverse warp + exact cell-risk fallback; live cost disabled"',
    'L"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow; validated jump-flood dense flow + edge-aware next-frame warp; live cost disabled"')

$dispatchPattern = '(?s)\tbool DispatchMidpoint\(const float midpointTime\)\r?\n\t\{.*?\r?\n\t\}\r?\n\r?\n\tbool BeginInputFrame'
$dispatchReplacement = @'
	bool DispatchMidpoint(const float midpointTime)
	{
		if (!denseSynthesizer) {
			status = L"Native NVOF dense synthesizer is unavailable";
			return false;
		}
		return denseSynthesizer->Dispatch(
			context,
			inputs[currentIndex].view,
			inputs[writeIndex].view,
			forwardFlow.view,
			backwardFlow.view,
			outputUav,
			midpointTime,
			status);
	}

	bool BeginInputFrame
'@
$patched = [regex]::Replace($source, $dispatchPattern, $dispatchReplacement, 1)
if ($patched -eq $source -and -not $source.Contains('Native NVOF dense synthesizer is unavailable')) {
    throw 'Could not replace DispatchMidpoint.'
}
[IO.File]::WriteAllText($nativePath, $patched)

$projectPath = 'Source/MpcVideoRenderer.vcxproj'
$project = [IO.File]::ReadAllText($projectPath)
if (-not $project.Contains('NvidiaOpticalFlowDenseSynthesizer.cpp')) {
    $project = $project.Replace(
        '    <ClCompile Include="NvidiaOpticalFlowCapture.cpp" />',
        "    <ClCompile Include=`"NvidiaOpticalFlowCapture.cpp`" />`r`n    <ClCompile Include=`"NvidiaOpticalFlowDenseSynthesizer.cpp`" />")
}
if (-not $project.Contains('NvidiaOpticalFlowDenseSynthesizer.h')) {
    $project = $project.Replace(
        '    <ClInclude Include="NvidiaOpticalFlowCapture.h" />',
        "    <ClInclude Include=`"NvidiaOpticalFlowCapture.h`" />`r`n    <ClInclude Include=`"NvidiaOpticalFlowDenseSynthesizer.h`" />")
}
[IO.File]::WriteAllText($projectPath, $project)

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add Source/NvidiaOpticalFlowDense* Source/NvidiaOpticalFlowNative.cpp Source/MpcVideoRenderer.vcxproj
git commit -m 'Prototype FRUC-like dense NVOF reconstruction'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git push origin HEAD:$branch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
