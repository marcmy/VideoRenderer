#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$MediaPath,
    [string]$PlayerPath,
    [ValidateSet('Balanced', 'Smoothness', 'Quality')]
    [string]$Priority = 'Balanced',
    [ValidateRange(1, 12)]
    [int]$MaxAttempts = 6,
    [ValidateRange(0, 60)]
    [int]$WarmupSeconds = 3,
    [ValidateRange(3, 300)]
    [int]$DurationSeconds = 12,
    [ValidateRange(25, 2000)]
    [int]$SampleIntervalMilliseconds = 100,
    [ValidateRange(-20.0, 20.0)]
    [double]$MinimumHeadroomMilliseconds = 1.5,
    [ValidateRange(0.1, 20.0)]
    [double]$FpsTolerancePercent = 3.0,
    [string]$ProfileName,
    [string]$ProfileRoot,
    [string]$RegistryPath = 'HKCU:\Software\MPCVideoRenderer',
    [string]$DataRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup'),
    [switch]$LockProfile,
    [switch]$Json,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 2.0

$autoTuneModule = Join-Path $PSScriptRoot 'MpcvrSetup.AutoTune.psm1'
$telemetryModule = Join-Path $PSScriptRoot 'MpcvrSetup.Telemetry.psm1'
$settingsModule = Join-Path $PSScriptRoot 'MpcvrSetup.RendererSettings.psm1'
$recommendationsModule = Join-Path $PSScriptRoot 'MpcvrSetup.Recommendations.psm1'
$profilesModule = Join-Path $PSScriptRoot 'MpcvrSetup.Profiles.psm1'
$commonModule = Join-Path $PSScriptRoot 'MpcvrSetup.Common.psm1'
$calibrationScript = Join-Path $PSScriptRoot 'Invoke-MpcvrCalibration.ps1'
foreach ($path in @(
    $autoTuneModule,
    $telemetryModule,
    $settingsModule,
    $recommendationsModule,
    $profilesModule,
    $commonModule,
    $calibrationScript
)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Auto-tune dependency is missing: $path"
    }
}

Import-Module -Name $autoTuneModule -Force
Import-Module -Name $telemetryModule -Force
Import-Module -Name $settingsModule -Force
Import-Module -Name $recommendationsModule -Force
Import-Module -Name $profilesModule -Force
Import-Module -Name $commonModule -Force

if ($ValidateOnly) {
    if (-not (Test-MpcvrAutoTunePlanner)) {
        throw 'The automatic calibration candidate planner failed its self-test.'
    }
    if (-not (Test-MpcvrTelemetryReader)) {
        throw 'The automatic calibration telemetry reader failed its self-test.'
    }
    Write-Host 'MPCVR automatic calibration loop validation passed.' -ForegroundColor Green
    exit 0
}

if ([string]::IsNullOrWhiteSpace($MediaPath)) {
    throw 'MediaPath is required for automatic calibration.'
}
$resolvedMediaPath = (Resolve-Path -LiteralPath $MediaPath).Path
if (-not (Test-Path -LiteralPath $resolvedMediaPath -PathType Leaf)) {
    throw "Calibration media file was not found: $resolvedMediaPath"
}
$resolvedPlayerPath = Resolve-MpcvrPlayerPath -PlayerPath $PlayerPath
if (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
    throw 'Close every existing MPC-HC process before automatic calibration.'
}

$DataRoot = [IO.Path]::GetFullPath($DataRoot)
$runId = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), ([guid]::NewGuid().ToString('N').Substring(0, 8))
$runRoot = Join-Path $DataRoot (Join-Path 'auto-tune' $runId)
$reportsRoot = Join-Path $runRoot 'calibrations'
$settingsBackupsRoot = Join-Path $runRoot 'settings-backups'
New-Item -ItemType Directory -Path $reportsRoot -Force | Out-Null
New-Item -ItemType Directory -Path $settingsBackupsRoot -Force | Out-Null

$originalSettings = Get-MpcvrRendererSettings -RegistryPath $RegistryPath
$originalBackupPath = Save-MpcvrRendererSettingsBackup `
    -Snapshot $originalSettings `
    -Path (Join-Path $runRoot 'original-renderer-settings.json')
$triedSignatures = @()
$attempts = @()
$ownedPlayer = $null
$success = $false
$restoredOriginal = $false
$savedProfilePath = $null
$failureMessage = $null

try {
    for ($attemptNumber = 1; $attemptNumber -le $MaxAttempts; $attemptNumber++) {
        Write-Host
        Write-Host "Automatic calibration attempt $attemptNumber of $MaxAttempts" -ForegroundColor Cyan
        $ownedPlayer = Start-MpcvrOwnedPlayer `
            -PlayerPath $resolvedPlayerPath `
            -MediaPath $resolvedMediaPath

        $reportPath = Join-Path $reportsRoot ('attempt-{0:D2}.json' -f $attemptNumber)
        try {
            $telemetry = @(Get-MpcvrRendererTelemetry `
                -ProcessId ([uint32]$ownedPlayer.Id) `
                -WaitSeconds 20 `
                -PollIntervalMilliseconds 100)
            if ($telemetry.Count -eq 0) {
                throw 'MPC-HC started, but the custom renderer telemetry did not appear. Confirm that MPC Video Renderer is selected.'
            }

            & $calibrationScript `
                -ProcessId ([uint32]$ownedPlayer.Id) `
                -WarmupSeconds $WarmupSeconds `
                -DurationSeconds $DurationSeconds `
                -SampleIntervalMilliseconds $SampleIntervalMilliseconds `
                -MinimumHeadroomMilliseconds $MinimumHeadroomMilliseconds `
                -FpsTolerancePercent $FpsTolerancePercent `
                -OutputPath $reportPath `
                -Json | Out-Null
        }
        finally {
            Stop-MpcvrOwnedPlayer -Process $ownedPlayer
            $ownedPlayer = $null
        }

        if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
            throw "Calibration attempt $attemptNumber did not create a report."
        }
        $calibration = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        $currentSettings = Get-MpcvrRendererSettings -RegistryPath $RegistryPath
        $recommendation = Get-MpcvrCalibrationRecommendation `
            -Calibration $calibration `
            -CurrentSettings $currentSettings `
            -Priority $Priority

        $attemptRecord = [ordered]@{
            Attempt = $attemptNumber
            CalibrationPath = $reportPath
            Verdict = [string]$calibration.Verdict
            SettingsSignature = Get-MpcvrSettingsSignature -Settings $currentSettings.Settings
            RecommendationId = [string]$recommendation.RecommendedCandidateId
            AppliedCandidateId = $null
            AppliedSettingsSignature = $null
        }

        if ([string]$calibration.Verdict -eq 'Stable') {
            $stableCandidate = @($recommendation.Candidates |
                Where-Object { $_.Id -eq 'keep-measured' } |
                Select-Object -First 1)
            if ($stableCandidate.Count -eq 0) {
                throw 'Stable calibration did not produce a keep-measured candidate.'
            }

            if ([string]::IsNullOrWhiteSpace($ProfileName)) {
                $ProfileName = 'Automatic {0:N2} fps - {1}' -f `
                    ([double]$calibration.Workload.TargetOutputFpsMedian), `
                    (Get-Date -Format 'yyyy-MM-dd HHmm')
            }
            $systemProfile = Get-MpcvrSystemProfile
            $profile = New-MpcvrProfileFromRecommendation `
                -Candidate $stableCandidate[0] `
                -Calibration $calibration `
                -Name $ProfileName `
                -Mode Automatic `
                -SystemProfile $systemProfile
            $profile.locked = [bool]$LockProfile
            $savedProfilePath = Save-MpcvrProfile `
                -Profile $profile `
                -ProfileRoot $ProfileRoot

            $attempts += [pscustomobject]$attemptRecord
            $success = $true
            break
        }

        if ($attemptNumber -ge $MaxAttempts) {
            $attempts += [pscustomobject]$attemptRecord
            break
        }

        $selection = Select-MpcvrAutoTuneCandidate `
            -Recommendation $recommendation `
            -TriedSignatures $triedSignatures
        if ($null -eq $selection) {
            $attempts += [pscustomobject]$attemptRecord
            break
        }

        $triedSignatures += [string]$selection.Signature
        $attemptRecord['AppliedCandidateId'] = [string]$selection.Candidate.Id
        $attemptRecord['AppliedSettingsSignature'] = [string]$selection.Signature
        $attempts += [pscustomobject]$attemptRecord

        [void](Set-MpcvrRendererSettings `
            -Settings (ConvertTo-MpcvrSettingsDictionary -SettingsObject $selection.Candidate.Settings) `
            -RegistryPath $RegistryPath `
            -BackupPath (Join-Path $settingsBackupsRoot ('before-attempt-{0:D2}.json' -f ($attemptNumber + 1))))
    }

    if (-not $success) {
        [void](Restore-MpcvrRendererSettingsBackup `
            -BackupPath $originalBackupPath `
            -RegistryPath $RegistryPath)
        $restoredOriginal = $true
    }
}
catch {
    $failureMessage = $_.Exception.Message
    if ($null -ne $ownedPlayer) {
        Stop-MpcvrOwnedPlayer -Process $ownedPlayer
        $ownedPlayer = $null
    }
    try {
        [void](Restore-MpcvrRendererSettingsBackup `
            -BackupPath $originalBackupPath `
            -RegistryPath $RegistryPath)
        $restoredOriginal = $true
    }
    catch {
        $failureMessage += " Original-settings restore also failed: $($_.Exception.Message)"
    }
}
finally {
    if ($null -ne $ownedPlayer) {
        Stop-MpcvrOwnedPlayer -Process $ownedPlayer
    }
}

$runResult = [pscustomobject]@{
    SchemaVersion = 1
    RunId = $runId
    CompletedAtUtc = [DateTime]::UtcNow.ToString('o')
    Success = $success
    Priority = $Priority
    MediaPath = $resolvedMediaPath
    PlayerPath = $resolvedPlayerPath
    MaxAttempts = $MaxAttempts
    Attempts = $attempts
    OriginalSettingsBackup = $originalBackupPath
    RestoredOriginalSettings = $restoredOriginal
    ProfilePath = $savedProfilePath
    Error = $failureMessage
}
$runReportPath = Join-Path $runRoot 'auto-tune-result.json'
$runResult | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath $runReportPath -Encoding UTF8

if ($Json) {
    $runResult | ConvertTo-Json -Depth 14
}
else {
    Write-Host
    if ($success) {
        Write-Host 'Automatic calibration found a stable configuration.' -ForegroundColor Green
        Write-Host "Profile: $savedProfilePath"
    }
    else {
        Write-Host 'Automatic calibration did not find a passing configuration.' -ForegroundColor Yellow
        Write-Host 'The original renderer settings were restored.'
    }
    if (-not [string]::IsNullOrWhiteSpace($failureMessage)) {
        Write-Host "Error: $failureMessage" -ForegroundColor Red
    }
    Write-Host "Run report: $runReportPath"
}

if (-not [string]::IsNullOrWhiteSpace($failureMessage)) {
    exit 1
}
if (-not $success) {
    exit 2
}
