$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$path = Join-Path $root 'Source\NvidiaOpticalFlowDenseSeed.hlsl'
if (-not (Test-Path $path)) {
    throw "Missing shader source: $path"
}

$text = (Get-Content -LiteralPath $path -Raw).Replace("`r`n", "`n")

if ($text.Contains('float LocalTemporalChange(int2 cell)') -and
    $text.Contains('&& localTemporalChange >= 0.030')) {
    Write-Host 'NVOF temporal-motion salvage gate is already applied.'
    exit 0
}

$insertNeedle = 'static const uint BackwardSeedBit = 0x80000000u;'
$insertReplacement = @'
float TemporalChangeAtCell(int2 cell)
{
    cell = clamp(cell, int2(0, 0), int2(FlowSize) - 1);
    float2 samplePixel = float2(cell) * GridSize;
    float3 previousRgb = SampleFrame(PreviousFrame, samplePixel).rgb;
    float3 nextRgb = SampleFrame(NextFrame, samplePixel).rgb;
    return dot(
        abs(previousRgb - nextRgb),
        float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
}

float LocalTemporalChange(int2 cell)
{
    // A bogus raw flow vector can be very large even in visually slower
    // material, so vector magnitude is not reliable evidence that salvage is
    // needed. Require actual source-frame change in a small local cross instead.
    return (
        TemporalChangeAtCell(cell) +
        TemporalChangeAtCell(cell + int2(-1, 0)) +
        TemporalChangeAtCell(cell + int2(1, 0)) +
        TemporalChangeAtCell(cell + int2(0, -1)) +
        TemporalChangeAtCell(cell + int2(0, 1))) / 5.0;
}

static const uint BackwardSeedBit = 0x80000000u;
'@

if (-not $text.Contains($insertNeedle)) {
    throw 'Could not locate NVOF salvage helper insertion point.'
}
$text = $text.Replace($insertNeedle, $insertReplacement.TrimEnd())

$gatePattern = '(?ms)^    bool forwardSalvage = neitherConsistencyValid\n\s*&& previousPixelInBounds\n\s*&& bToAPhotoError <= 0\.025\n\s*&& ForwardMidpointNearlyRigid\(cell\);'
$gateReplacement = @'
    float localTemporalChange = neitherConsistencyValid
        ? LocalTemporalChange(cell)
        : 0.0;

    bool forwardSalvage = neitherConsistencyValid
        && previousPixelInBounds
        && bToAPhotoError <= 0.025
        // Preserve the fast-motion win without letting color-plausible bogus
        // vectors take over slower material. Offline corpus replay showed that
        // a 3% local source-frame MAD keeps useful Boromir-fling salvage while
        // removing most salvage from the slower/problematic captures.
        && localTemporalChange >= 0.030
        && ForwardMidpointNearlyRigid(cell);
'@

$matches = [regex]::Matches($text, $gatePattern)
if ($matches.Count -ne 1) {
    throw "Expected exactly one conservative forward-salvage gate, found $($matches.Count)."
}
$text = [regex]::Replace($text, $gatePattern, $gateReplacement.TrimEnd(), 1)

[IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
Write-Host 'Applied NVOF local temporal-motion salvage gate (5-point MAD >= 0.030).'
