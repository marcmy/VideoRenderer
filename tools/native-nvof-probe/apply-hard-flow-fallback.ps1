$ErrorActionPreference = 'Stop'

$sourcePath = 'Source/NvidiaOpticalFlowNative.cpp'
$source = [IO.File]::ReadAllText($sourcePath)
if ($source.Contains('Hard safety fallback for fast motion')) {
    Write-Host 'Hard flow-consistency fallback already landed.'
    exit 0
}

$needle = @'
    OutputFrame[id.xy] = result;
}
)hlsl";
'@
$replacement = @'
    // Hard safety fallback for fast motion. If both optical-flow directions
    // fail their round-trip consistency test by a large margin, do not blend
    // or further warp the pixel. Reuse the nearer real endpoint instead.
    // This trades a local half-frame repeat for avoiding severe geometry melt.
    float2 firstFlowAtPixel = SampleFlow(true, pixel);
    float2 secondFlowAtPixel = SampleFlow(false, pixel);
    float localMotion = max(length(firstFlowAtPixel), length(secondFlowAtPixel));

    float firstRoundTripError = 1000.0;
    float2 firstMatch = pixel + firstFlowAtPixel;
    if (IsValid(firstMatch)) {
        firstRoundTripError = length(firstFlowAtPixel + SampleFlow(false, firstMatch));
    }

    float secondRoundTripError = 1000.0;
    float2 secondMatch = pixel + secondFlowAtPixel;
    if (IsValid(secondMatch)) {
        secondRoundTripError = length(secondFlowAtPixel + SampleFlow(true, secondMatch));
    }

    if (localMotion >= 20.0 && min(firstRoundTripError, secondRoundTripError) >= 20.0) {
        result = MidpointTime <= 0.5
            ? SampleFrame(FirstFrame, pixel)
            : SampleFrame(SecondFrame, pixel);
    }

    OutputFrame[id.xy] = result;
}
)hlsl";
'@
if (-not $source.Contains($needle)) {
    throw 'Could not find midpoint shader output marker.'
}
$source = $source.Replace($needle, $replacement)
[IO.File]::WriteAllText($sourcePath, $source)

$sdkBin = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$fxc = Get-ChildItem $sdkBin -Filter fxc.exe -Recurse -File |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $fxc) { throw 'fxc.exe was not found in the Windows SDK.' }

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

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add Source/NvidiaOpticalFlowNative.cpp Source/NvidiaOpticalFlowMidpointBytecode.h
git commit -m 'Reject severely inconsistent fast-motion warps'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git push origin HEAD:feature/native-nvof-interpolation
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
