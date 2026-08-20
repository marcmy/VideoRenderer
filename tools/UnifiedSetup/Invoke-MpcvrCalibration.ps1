#requires -Version 5.1

[CmdletBinding()]
param(
    [uint32]$ProcessId,
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
    [ValidateSet('Automatic', 'Guided', 'Advanced')]
    [string]$Mode = 'Automatic',
    [string]$ProfileRoot,
    [string]$OutputPath,
    [switch]$AllowLockedOverwrite,
    [switch]$Json,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$telemetryModule = Join-Path $PSScriptRoot 'MpcvrSetup.Telemetry.psm1'
$profilesModule = Join-Path $PSScriptRoot 'MpcvrSetup.Profiles.psm1'
$commonModule = Join-Path $PSScriptRoot 'MpcvrSetup.Common.psm1'
foreach ($path in @($telemetryModule, $profilesModule, $commonModule)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Calibration dependency is missing: $path"
    }
}

Import-Module -Name $telemetryModule -Force
Import-Module -Name $profilesModule -Force
Import-Module -Name $commonModule -Force

function Get-Percentile {
    param(
        [Parameter(Mandatory)]
        [double[]]$Values,
        [ValidateRange(0.0, 1.0)]
        [double]$Percentile
    )

    $sorted = @($Values | Where-Object { [double]::IsNaN($_) -eq $false -and [double]::IsInfinity($_) -eq $false } | Sort-Object)
    if ($sorted.Count -eq 0) {
        return 0.0
    }
    if ($sorted.Count -eq 1) {
        return [double]$sorted[0]
    }

    $position = ($sorted.Count - 1) * $Percentile
    $lower = [int][math]::Floor($position)
    $upper = [int][math]::Ceiling($position)
    if ($lower -eq $upper) {
        return [double]$sorted[$lower]
    }
    $weight = $position - $lower
    return ([double]$sorted[$lower] * (1.0 - $weight)) + ([double]$sorted[$upper] * $weight)
}

function Get-Delta {
    param(
        [uint64]$First,
        [uint64]$Last
    )

    if ($Last -ge $First) {
        return $Last - $First
    }
    return 0
}

function Set-OrAddProperty {
    param(
        [Parameter(Mandatory)]
        [object]$Object,
        [Parameter(Mandatory)]
        [string]$Name,
        $Value
    )

    if (@($Object.PSObject.Properties.Name) -contains $Name) {
        $Object.$Name = $Value
    }
    else {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    }
}

if ($ValidateOnly) {
    if (-not (Test-MpcvrTelemetryReader)) {
        throw 'Telemetry reader self-test failed.'
    }
    $synthetic = @(1.0, 2.0, 3.0, 4.0, 5.0)
    if ((Get-Percentile -Values $synthetic -Percentile 0.5) -ne 3.0 -or
        (Get-Percentile -Values $synthetic -Percentile 0.95) -le 4.0) {
        throw 'Calibration percentile helper self-test failed.'
    }
    Write-Host 'MPCVR local calibration validation passed.' -ForegroundColor Green
    exit 0
}

$firstCandidates = @(Get-MpcvrRendererTelemetry -ProcessId $ProcessId -WaitSeconds 10)
if ($firstCandidates.Count -eq 0) {
    throw 'No MPCVR telemetry was found. Start video playback before calibration.'
}
$first = $firstCandidates | Sort-Object AgeMilliseconds | Select-Object -First 1
$selectedProcessId = [uint32]$first.ProcessId

if (-not $first.Fresh -or -not $first.RendererActive) {
    throw 'The renderer telemetry is stale or playback is not active.'
}

if (-not [string]::IsNullOrWhiteSpace($ProfileName)) {
    try {
        $existingProfile = (Find-MpcvrProfile -Name $ProfileName -ProfileRoot $ProfileRoot).Profile
        if ([bool]$existingProfile.locked -and -not $AllowLockedOverwrite) {
            throw "Profile '$ProfileName' is locked. Use AllowLockedOverwrite only after explicit user approval."
        }
    }
    catch {
        if ($_.Exception.Message -notmatch '^Profile was not found:') {
            throw
        }
    }
}

Write-Host "Calibrating MPCVR process $selectedProcessId..." -ForegroundColor Cyan
Write-Host "Warm-up: $WarmupSeconds s; measurement: $DurationSeconds s"

$warmupDeadline = [DateTime]::UtcNow.AddSeconds($WarmupSeconds)
do {
    $warmupSample = @(Get-MpcvrRendererTelemetry -ProcessId $selectedProcessId | Select-Object -First 1)
    if ($warmupSample.Count -eq 0 -or -not $warmupSample[0].Fresh) {
        throw 'Renderer telemetry disappeared during warm-up.'
    }
    Start-Sleep -Milliseconds $SampleIntervalMilliseconds
} while ([DateTime]::UtcNow -lt $warmupDeadline)

$samples = @()
$measurementDeadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
do {
    $sample = @(Get-MpcvrRendererTelemetry -ProcessId $selectedProcessId | Select-Object -First 1)
    if ($sample.Count -eq 0) {
        throw 'Renderer telemetry disappeared during calibration.'
    }
    if ($sample[0].Fresh -and $sample[0].RendererActive) {
        $samples += $sample[0]
    }
    Start-Sleep -Milliseconds $SampleIntervalMilliseconds
} while ([DateTime]::UtcNow -lt $measurementDeadline)

if ($samples.Count -lt 10) {
    throw "Calibration collected too few valid samples: $($samples.Count)."
}

$firstSample = $samples[0]
$lastSample = $samples[-1]
$sourceFpsMedian = Get-Percentile -Values @($samples.SourceFps) -Percentile 0.5
$targetFpsMedian = Get-Percentile -Values @($samples.TargetOutputFps) -Percentile 0.5
$drawFpsMedian = Get-Percentile -Values @($samples.MeasuredDrawFps) -Percentile 0.5
$drawFpsP10 = Get-Percentile -Values @($samples.MeasuredDrawFps) -Percentile 0.10
$maxineMedian = Get-Percentile -Values @($samples.MaxineTotalMilliseconds) -Percentile 0.5
$maxineP95 = Get-Percentile -Values @($samples.MaxineTotalMilliseconds) -Percentile 0.95
$frucMedian = Get-Percentile -Values @($samples.FrameInterpolationMilliseconds) -Percentile 0.5
$frucP95 = Get-Percentile -Values @($samples.FrameInterpolationMilliseconds) -Percentile 0.95
$combinedMedian = Get-Percentile -Values @($samples.CombinedProcessingMilliseconds) -Percentile 0.5
$combinedP95 = Get-Percentile -Values @($samples.CombinedProcessingMilliseconds) -Percentile 0.95
$headroomMedian = Get-Percentile -Values @($samples.TimingHeadroomMilliseconds) -Percentile 0.5
$headroomP05 = Get-Percentile -Values @($samples.TimingHeadroomMilliseconds) -Percentile 0.05
$budgetMedian = Get-Percentile -Values @($samples.SourceFrameBudgetMilliseconds) -Percentile 0.5

$droppedDelta = Get-Delta -First $firstSample.DroppedFrames -Last $lastSample.DroppedFrames
$skippedDelta = Get-Delta -First $firstSample.SkippedFrames -Last $lastSample.SkippedFrames
$failedDelta = Get-Delta -First $firstSample.FailedFrames -Last $lastSample.FailedFrames
$frameDelta = Get-Delta -First $firstSample.Frames -Last $lastSample.Frames

$minimumMedianFps = $targetFpsMedian * (1.0 - ($FpsTolerancePercent / 100.0))
$minimumP10Fps = $targetFpsMedian * (1.0 - (($FpsTolerancePercent * 2.0) / 100.0))
$stablePacing = $targetFpsMedian -gt 0.0 -and
    $drawFpsMedian -ge $minimumMedianFps -and
    $drawFpsP10 -ge $minimumP10Fps
$noFrameFailures = $droppedDelta -eq 0 -and $skippedDelta -eq 0 -and $failedDelta -eq 0
$enoughHeadroom = $headroomP05 -ge $MinimumHeadroomMilliseconds

$verdict = 'Unstable'
if ($stablePacing -and $noFrameFailures -and $enoughHeadroom) {
    $verdict = 'Stable'
}
elseif ($failedDelta -eq 0 -and
        $drawFpsMedian -ge ($targetFpsMedian * 0.95) -and
        $headroomP05 -ge 0.0) {
    $verdict = 'Marginal'
}

$result = [pscustomobject]@{
    SchemaVersion = 1
    CapturedAtUtc = [DateTime]::UtcNow.ToString('o')
    ProcessId = $selectedProcessId
    DurationSeconds = $DurationSeconds
    SampleIntervalMilliseconds = $SampleIntervalMilliseconds
    SampleCount = $samples.Count
    Verdict = $verdict
    Requirements = [pscustomobject]@{
        MinimumHeadroomMilliseconds = $MinimumHeadroomMilliseconds
        FpsTolerancePercent = $FpsTolerancePercent
    }
    Workload = [pscustomobject]@{
        SourceWidth = [uint32]$lastSample.SourceWidth
        SourceHeight = [uint32]$lastSample.SourceHeight
        OutputWidth = [uint32]$lastSample.OutputWidth
        OutputHeight = [uint32]$lastSample.OutputHeight
        SourceFpsMedian = $sourceFpsMedian
        TargetOutputFpsMedian = $targetFpsMedian
        MaxineEnabled = [bool]$lastSample.MaxineEnabled
        MaxineActive = [bool]$lastSample.MaxineActive
        FrameInterpolationEnabled = [bool]$lastSample.FrameInterpolationEnabled
        FrameInterpolationActive = [bool]$lastSample.FrameInterpolationActive
        CombinedActive = [bool]$lastSample.CombinedActive
    }
    Timing = [pscustomobject]@{
        SourceFrameBudgetMillisecondsMedian = $budgetMedian
        MaxineMillisecondsMedian = $maxineMedian
        MaxineMillisecondsP95 = $maxineP95
        FrameInterpolationMillisecondsMedian = $frucMedian
        FrameInterpolationMillisecondsP95 = $frucP95
        CombinedMillisecondsMedian = $combinedMedian
        CombinedMillisecondsP95 = $combinedP95
        HeadroomMillisecondsMedian = $headroomMedian
        HeadroomMillisecondsP05 = $headroomP05
    }
    Pacing = [pscustomobject]@{
        MeasuredDrawFpsMedian = $drawFpsMedian
        MeasuredDrawFpsP10 = $drawFpsP10
        Stable = $stablePacing
    }
    Frames = [pscustomobject]@{
        SourceFramesObserved = $frameDelta
        Dropped = $droppedDelta
        Skipped = $skippedDelta
        Failed = $failedDelta
    }
    RendererSettings = [pscustomobject]@{
        MaxineOperation = [int]$lastSample.MaxineOperation
        MaxineQuality = [int]$lastSample.MaxineQuality
        MaxineScale = [int]$lastSample.MaxineScale
        MaxineOversample = [int]$lastSample.MaxineOversample
        FrameInterpolationMode = [int]$lastSample.FrameInterpolationMode
        FrameInterpolationSourceLimit = [int]$lastSample.FrameInterpolationSourceLimit
        FrameInterpolationMaxOutput = [int]$lastSample.FrameInterpolationMaxOutput
    }
    ProfilePath = $null
    OutputPath = $null
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $calibrationRoot = Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup\calibrations'
    New-Item -ItemType Directory -Path $calibrationRoot -Force | Out-Null
    $OutputPath = Join-Path $calibrationRoot (
        'calibration-{0}-{1}.json' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $selectedProcessId)
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [string]::IsNullOrWhiteSpace($parent)) {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
}
$result.OutputPath = $OutputPath

if (-not [string]::IsNullOrWhiteSpace($ProfileName)) {
    $sourceClass = if ($sourceFpsMedian -le 30.5) { 'up-to-30' } elseif ($sourceFpsMedian -le 60.5) { 'over-30-to-60' } else { 'custom' }
    try {
        $profile = (Find-MpcvrProfile -Name $ProfileName -ProfileRoot $ProfileRoot).Profile
        $profileExists = $true
    }
    catch {
        if ($_.Exception.Message -notmatch '^Profile was not found:') {
            throw
        }
        $profile = New-MpcvrProfile -Name $ProfileName -Mode $Mode -SourceFrameRateClass $sourceClass
        $profileExists = $false
    }

    $system = Get-MpcvrSystemProfile
    $gpu = @($system.Gpus | Select-Object -First 1)
    if ($gpu.Count -gt 0) {
        $profile.machineFingerprint.gpuName = [string]$gpu[0].Name
        $profile.machineFingerprint.gpuPnpDeviceId = [string]$gpu[0].PnpDeviceId
        $profile.machineFingerprint.driverVersion = [string]$gpu[0].DriverVersion
        $profile.machineFingerprint.displayWidth = [int]$gpu[0].CurrentWidth
        $profile.machineFingerprint.displayHeight = [int]$gpu[0].CurrentHeight
        $profile.machineFingerprint.displayRefreshHz = [double]$gpu[0].CurrentRefreshHz
    }

    $profile.conditions.sourceFrameRateClass = $sourceClass
    $profile.conditions.sourceWidthMax = [int]$lastSample.SourceWidth
    $profile.conditions.sourceHeightMax = [int]$lastSample.SourceHeight
    $profile.conditions.outputWidth = [int]$lastSample.OutputWidth
    $profile.conditions.outputHeight = [int]$lastSample.OutputHeight
    $profile.conditions.targetOutputFps = $targetFpsMedian
    $profile.maxine.enabled = [bool]$lastSample.MaxineEnabled
    $profile.frameInterpolation.enabled = [bool]$lastSample.FrameInterpolationEnabled
    $profile.frameInterpolation.maxOutputFps = $targetFpsMedian

    $profile.calibration.capturedAtUtc = $result.CapturedAtUtc
    $profile.calibration.durationSeconds = $DurationSeconds
    $profile.calibration.maxineMilliseconds = $maxineMedian
    $profile.calibration.frameInterpolationMilliseconds = $frucMedian
    $profile.calibration.totalMilliseconds = $combinedMedian
    $profile.calibration.timingHeadroomMilliseconds = $headroomP05
    $profile.calibration.droppedFrames = [int]$droppedDelta
    $profile.calibration.failedFrames = [int]$failedDelta
    $profile.calibration.pacingStable = $stablePacing
    Set-OrAddProperty -Object $profile.calibration -Name 'sampleCount' -Value $samples.Count
    Set-OrAddProperty -Object $profile.calibration -Name 'verdict' -Value $verdict
    Set-OrAddProperty -Object $profile.calibration -Name 'combinedMillisecondsP95' -Value $combinedP95
    Set-OrAddProperty -Object $profile.calibration -Name 'headroomMillisecondsP05' -Value $headroomP05
    Set-OrAddProperty -Object $profile.calibration -Name 'measuredDrawFpsMedian' -Value $drawFpsMedian
    Set-OrAddProperty -Object $profile.calibration -Name 'measuredDrawFpsP10' -Value $drawFpsP10

    $result.ProfilePath = Save-MpcvrProfile `
        -Profile $profile `
        -ProfileRoot $ProfileRoot `
        -Force:$profileExists `
        -AllowLockedOverwrite:$AllowLockedOverwrite
}

$result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $OutputPath -Encoding UTF8

if ($Json) {
    $result | ConvertTo-Json -Depth 12
}
else {
    Write-Host
    Write-Host "Calibration verdict: $verdict" -ForegroundColor $(if ($verdict -eq 'Stable') { 'Green' } elseif ($verdict -eq 'Marginal') { 'Yellow' } else { 'Red' })
    Write-Host ('Source/target/draw: {0:N3} / {1:N3} / {2:N3} fps' -f $sourceFpsMedian, $targetFpsMedian, $drawFpsMedian)
    Write-Host ('Maxine/FRUC/combined P95: {0:N2} / {1:N2} / {2:N2} ms' -f $maxineP95, $frucP95, $combinedP95)
    Write-Host ('Headroom P05: {0:N2} ms' -f $headroomP05)
    Write-Host "Dropped/skipped/failed: $droppedDelta / $skippedDelta / $failedDelta"
    Write-Host "Report: $OutputPath"
    if (-not [string]::IsNullOrWhiteSpace([string]$result.ProfilePath)) {
        Write-Host "Profile: $($result.ProfilePath)"
    }
}
