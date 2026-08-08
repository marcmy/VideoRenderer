#requires -Version 5.1

[CmdletBinding()]
param(
    [uint32]$ProcessId,
    [ValidateRange(0, 300)]
    [int]$WaitSeconds = 0,
    [ValidateRange(10, 5000)]
    [int]$PollIntervalMilliseconds = 100,
    [switch]$Json,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$modulePath = Join-Path $PSScriptRoot 'MpcvrSetup.Telemetry.psm1'
if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    throw "Renderer telemetry module is missing: $modulePath"
}
Import-Module -Name $modulePath -Force

if ($ValidateOnly) {
    if (-not (Test-MpcvrTelemetryReader)) {
        throw 'The renderer telemetry shared-memory reader failed its self-test.'
    }
    Write-Host 'Renderer telemetry reader validation passed.' -ForegroundColor Green
    exit 0
}

$results = @(Get-MpcvrRendererTelemetry `
    -ProcessId $ProcessId `
    -WaitSeconds $WaitSeconds `
    -PollIntervalMilliseconds $PollIntervalMilliseconds)

if ($results.Count -eq 0) {
    throw 'No MPC Video Renderer calibration telemetry was found. Start playback with the unified renderer build and try again.'
}

if ($Json) {
    $results | ConvertTo-Json -Depth 6
}
else {
    $results | Format-List
}
