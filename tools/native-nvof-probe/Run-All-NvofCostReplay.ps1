#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$CaptureRoot
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$exe = Join-Path $PSScriptRoot 'NativeNvofCostReplay.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "NativeNvofCostReplay.exe was not found next to this script: $exe"
}

if (-not $CaptureRoot) {
    $CaptureRoot = Read-Host 'Root folder containing MPCVR NVOF capture folders'
}
if (-not $CaptureRoot) {
    throw 'No capture root was supplied.'
}
$CaptureRoot = (Resolve-Path -LiteralPath $CaptureRoot).Path

$frameA = @(Get-ChildItem -LiteralPath $CaptureRoot -Filter 'frame-A.bmp' -File -Recurse -ErrorAction Stop |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.Directory.FullName 'frame-B.bmp') })
if (-not $frameA.Count) {
    throw "No capture directories containing frame-A.bmp + frame-B.bmp were found under $CaptureRoot"
}

Write-Host "Found $($frameA.Count) capture pair(s)."
Write-Host

$started = Get-Date
$results = @()
foreach ($item in $frameA) {
    $capture = $item.Directory.FullName
    $name = $item.Directory.Name
    Write-Host "=== $name ===" -ForegroundColor Cyan

    $before = @(Get-ChildItem -LiteralPath $capture -Directory -Filter 'nvof-cost-replay-*' -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName)

    & $exe $capture
    $rc = $LASTEXITCODE

    $after = @(Get-ChildItem -LiteralPath $capture -Directory -Filter 'nvof-cost-replay-*' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    $newOutput = $after | Where-Object { $before -notcontains $_.FullName } | Select-Object -First 1

    $results += [pscustomobject]@{
        Capture = $name
        ExitCode = $rc
        Output = if ($newOutput) { $newOutput.FullName } else { $null }
    }
    if ($rc -ne 0) {
        Write-Warning "$name failed with exit code $rc; continuing with the remaining captures."
    }
    Write-Host
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stage = Join-Path $env:TEMP "NVOF-Cost-Replay-Batch-$stamp"
$zip = Join-Path $CaptureRoot "NVOF-Cost-Replay-Batch-$stamp.zip"
New-Item -ItemType Directory -Path $stage -Force | Out-Null

try {
    foreach ($result in $results) {
        if (-not $result.Output -or -not (Test-Path -LiteralPath $result.Output -PathType Container)) {
            continue
        }
        $destination = Join-Path $stage $result.Capture
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        foreach ($fileName in @(
            'cost-forward-B-to-A-r8.bin',
            'cost-backward-A-to-B-r8.bin',
            'flow-forward-B-to-A-s10.5.bin',
            'flow-backward-A-to-B-s10.5.bin',
            'replay-summary.txt'
        )) {
            $source = Join-Path $result.Output $fileName
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Copy-Item -LiteralPath $source -Destination $destination -Force
            }
        }
    }

    $batchSummary = @(
        'NVOF hardware-cost replay batch'
        "Root=$CaptureRoot"
        "Started=$($started.ToString('o'))"
        "Finished=$((Get-Date).ToString('o'))"
        "Total=$($results.Count)"
        "Succeeded=$(($results | Where-Object ExitCode -eq 0).Count)"
        "Failed=$(($results | Where-Object ExitCode -ne 0).Count)"
        ''
    )
    foreach ($result in $results) {
        $batchSummary += "$($result.Capture)`texit=$($result.ExitCode)`toutput=$($result.Output)"
    }
    [IO.File]::WriteAllLines(
        (Join-Path $stage 'batch-summary.txt'),
        $batchSummary,
        [Text.UTF8Encoding]::new($false))

    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
}
finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host '============================================================' -ForegroundColor Green
Write-Host "Finished $($results.Count) capture(s): $(($results | Where-Object ExitCode -eq 0).Count) succeeded, $(($results | Where-Object ExitCode -ne 0).Count) failed."
Write-Host "Compact analysis bundle: $zip"
Write-Host 'The full replay folders, including grayscale cost BMPs, remain beside each capture.'

if (($results | Where-Object ExitCode -ne 0).Count -gt 0) {
    exit 1
}
exit 0
