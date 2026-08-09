$ErrorActionPreference = 'Stop'

$sourcePath = 'Source/NvidiaOpticalFlowNative.cpp'
$source = [IO.File]::ReadAllText($sourcePath)

if ($source.Contains('float2 SampleRawFlow(bool backward, float2 pixel)')) {
    Write-Host 'Raw-flow safety patch already applied.'
    exit 0
}

$needle = @'
float2 SampleFlow(bool backward, float2 pixel)
{
'@
$insert = @'
float2 SampleRawFlow(bool backward, float2 pixel)
{
    float2 grid = pixel / GridSize;
    grid = clamp(grid, 0.0, float2(FlowSize - 1));
    int2 p0 = int2(floor(grid));
    int2 p1 = min(p0 + 1, int2(FlowSize - 1));
    float2 fraction = grid - float2(p0);

    float2 f00 = float2(LoadFlowVector(backward, int2(p0.x, p0.y))) / 32.0;
    float2 f10 = float2(LoadFlowVector(backward, int2(p1.x, p0.y))) / 32.0;
    float2 f01 = float2(LoadFlowVector(backward, int2(p0.x, p1.y))) / 32.0;
    float2 f11 = float2(LoadFlowVector(backward, int2(p1.x, p1.y))) / 32.0;
    float2 top = lerp(f00, f10, fraction.x);
    float2 bottom = lerp(f01, f11, fraction.x);
    return lerp(top, bottom, fraction.y);
}

float2 SampleFlow(bool backward, float2 pixel)
{
'@
if (-not $source.Contains($needle)) { throw 'SampleFlow insertion point not found.' }
$source = $source.Replace($needle, $insert, 1)

$source = $source.Replace('float2 firstFlowAtPixel = SampleFlow(true, pixel);', 'float2 firstFlowAtPixel = SampleRawFlow(true, pixel);')
$source = $source.Replace('float2 secondFlowAtPixel = SampleFlow(false, pixel);', 'float2 secondFlowAtPixel = SampleRawFlow(false, pixel);')
$source = $source.Replace('firstRoundTripError = length(firstFlowAtPixel + SampleFlow(false, firstMatch));', 'firstRoundTripError = length(firstFlowAtPixel + SampleRawFlow(false, firstMatch));')
$source = $source.Replace('secondRoundTripError = length(secondFlowAtPixel + SampleFlow(true, secondMatch));', 'secondRoundTripError = length(secondFlowAtPixel + SampleRawFlow(true, secondMatch));')
$source = $source.Replace('min(firstRoundTripError, secondRoundTripError) >= 20.0', 'max(firstRoundTripError, secondRoundTripError) >= 20.0')
$source = $source.Replace('// fail their round-trip consistency test by a large margin, do not blend', '// expose a severe raw round-trip inconsistency in either direction, do not blend')
$source = $source.Replace('// This trades a local half-frame repeat for avoiding severe geometry melt.', '// Safety deliberately uses raw NVOF flow rather than the content-aware synthesis selector.\n    // This trades a local half-frame repeat for avoiding severe geometry melt.')

[IO.File]::WriteAllText($sourcePath, $source)

& .\tools\native-nvof-probe\Regenerate-NativeNvofShader.ps1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add Source/NvidiaOpticalFlowNative.cpp Source/NvidiaOpticalFlowMidpointBytecode.h
git commit -m 'Use raw NVOF flow for fast-motion safety gate'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git push origin HEAD:feature/native-nvof-interpolation
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
