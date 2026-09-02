#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$CalibrationPath,
    [ValidateSet('Balanced', 'Smoothness', 'Quality')]
    [string]$Priority = 'Balanced',
    [string]$RegistryPath = 'HKCU:\Software\MPCVideoRenderer',
    [string]$ProfileName,
    [ValidateSet('Automatic', 'Guided', 'Advanced')]
    [string]$Mode = 'Automatic',
    [string]$ProfileRoot,
    [string]$OutputPath,
    [switch]$SaveRecommendedProfile,
    [switch]$Force,
    [switch]$AllowLockedOverwrite,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$settingsModule = Join-Path $PSScriptRoot 'MpcvrSetup.RendererSettings.psm1'
$recommendationsModule = Join-Path $PSScriptRoot 'MpcvrSetup.Recommendations.psm1'
$profilesModule = Join-Path $PSScriptRoot 'MpcvrSetup.Profiles.psm1'
$commonModule = Join-Path $PSScriptRoot 'MpcvrSetup.Common.psm1'
foreach ($path in @($settingsModule, $recommendationsModule, $profilesModule, $commonModule)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Recommendation dependency is missing: $path"
    }
}
Import-Module -Name $settingsModule -Force
Import-Module -Name $recommendationsModule -Force
Import-Module -Name $profilesModule -Force
Import-Module -Name $commonModule -Force

$resolvedCalibrationPath = (Resolve-Path -LiteralPath $CalibrationPath).Path
$calibration = Get-Content -LiteralPath $resolvedCalibrationPath -Raw | ConvertFrom-Json
$currentSettings = Get-MpcvrRendererSettings -RegistryPath $RegistryPath
$recommendation = Get-MpcvrCalibrationRecommendation `
    -Calibration $calibration `
    -CurrentSettings $currentSettings `
    -Priority $Priority

$systemProfile = Get-MpcvrSystemProfile
$recommended = @($recommendation.Candidates | Where-Object { $_.Recommended } | Select-Object -First 1)
if ($recommended.Count -eq 0) {
    throw 'The recommendation engine did not select a candidate.'
}

$profilePath = $null
if ($SaveRecommendedProfile) {
    if ([string]::IsNullOrWhiteSpace($ProfileName)) {
        $sourceClass = [string]$calibration.Workload.SourceFpsMedian
        $ProfileName = 'Recommended {0} - {1} fps - {2}' -f `
            $Priority, `
            ([math]::Round([double]$calibration.Workload.TargetOutputFpsMedian, 2)), `
            (Get-Date -Format 'yyyy-MM-dd HHmm')
    }
    $profile = New-MpcvrProfileFromRecommendation `
        -Candidate $recommended[0] `
        -Calibration $calibration `
        -Name $ProfileName `
        -Mode $Mode `
        -SystemProfile $systemProfile
    $profilePath = Save-MpcvrProfile `
        -Profile $profile `
        -ProfileRoot $ProfileRoot `
        -Force:$Force `
        -AllowLockedOverwrite:$AllowLockedOverwrite
}

$result = [pscustomobject]@{
    SchemaVersion = 1
    GeneratedAtUtc = [DateTime]::UtcNow.ToString('o')
    CalibrationPath = $resolvedCalibrationPath
    RegistryPath = $RegistryPath
    ProfilePath = $profilePath
    Recommendation = $recommendation
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $outputRoot = Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup\recommendations'
    New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
    $OutputPath = Join-Path $outputRoot ('recommendation-{0}.json' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [string]::IsNullOrWhiteSpace($parent)) {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
}
$result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $OutputPath -Encoding UTF8

if ($Json) {
    $result | ConvertTo-Json -Depth 16
}
else {
    Write-Host
    Write-Host "Calibration verdict: $($recommendation.CalibrationVerdict)" -ForegroundColor Cyan
    Write-Host "Priority: $Priority"
    Write-Host "Recommended: $($recommended[0].Title)"
    Write-Host "Reason: $($recommended[0].Reason)"
    Write-Host "Requires recalibration: $($recommended[0].RequiresCalibration)"
    Write-Host
    $recommendation.Candidates |
        Select-Object Rank, Recommended, Id, Title, RequiresCalibration, Impact |
        Format-Table -Wrap -AutoSize
    Write-Host "Report: $OutputPath"
    if (-not [string]::IsNullOrWhiteSpace($profilePath)) {
        Write-Host "Saved profile: $profilePath"
    }
}
