#requires -Version 5.1

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Copy-MpcvrSettingsDictionary {
    param(
        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$Settings
    )

    $copy = [ordered]@{}
    foreach ($key in $Settings.Keys) {
        $copy[[string]$key] = [int64]$Settings[$key]
    }
    return $copy
}

function ConvertTo-MpcvrSettingsDictionary {
    param(
        [Parameter(Mandatory)]
        [object]$SettingsObject
    )

    $settings = [ordered]@{}
    foreach ($property in $SettingsObject.PSObject.Properties) {
        $settings[$property.Name] = [int64]$property.Value
    }
    return $settings
}

function Set-MpcvrMeasuredSettings {
    param(
        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$Settings,
        [Parameter(Mandatory)]
        [object]$Calibration
    )

    $measured = $Calibration.RendererSettings
    if ($null -eq $measured) {
        return
    }
    $mapping = [ordered]@{
        MaxineOperation = 'MaxineOperation'
        MaxineQuality = 'MaxineQuality'
        MaxineScale = 'MaxineScale'
        MaxineOversample = 'MaxineOversample'
        FrameInterpolationMode = 'FrameInterpolationMode'
        FrameInterpolationSourceLimit = 'FrameInterpolationSourceLimit'
        FrameInterpolationMaxOutput = 'FrameInterpolationMaxOutput'
    }
    foreach ($target in $mapping.Keys) {
        $property = $measured.PSObject.Properties |
            Where-Object { $_.Name -ieq $mapping[$target] } |
            Select-Object -First 1
        if ($null -ne $property) {
            $Settings[$target] = [int64]$property.Value
        }
    }
}

function Get-MpcvrLowerQuality {
    param([int64]$Quality)

    if ($Quality -gt 1) {
        return $Quality - 1
    }
    return 1
}

function Get-MpcvrLowerScale {
    param(
        [int64]$Scale,
        [double]$SourceWidth,
        [double]$OutputWidth
    )

    $tiers = @(133, 150, 200, 300, 400)
    if ($Scale -eq 0) {
        if ($SourceWidth -gt 0 -and $OutputWidth -gt 0) {
            $ratio = $OutputWidth / $SourceWidth
            if ($ratio -gt 3.0) { return 300 }
            if ($ratio -gt 2.0) { return 200 }
            if ($ratio -gt 1.5) { return 150 }
            if ($ratio -gt 1.333) { return 133 }
        }
        return $null
    }

    $lower = @($tiers | Where-Object { $_ -lt $Scale } | Sort-Object -Descending | Select-Object -First 1)
    if ($lower.Count -eq 0) {
        return $null
    }
    return [int64]$lower[0]
}

function Add-MpcvrRecommendationCandidate {
    param(
        [Parameter(Mandatory)]
        [ref]$Candidates,
        [Parameter(Mandatory)]
        [string]$Id,
        [Parameter(Mandatory)]
        [string]$Title,
        [Parameter(Mandatory)]
        [string]$Reason,
        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$Settings,
        [bool]$RequiresCalibration = $true,
        [string]$Impact = 'Unknown until recalibrated'
    )

    if (@($Candidates.Value | Where-Object { $_.Id -eq $Id }).Count -gt 0) {
        return
    }
    $Candidates.Value = @($Candidates.Value) + [pscustomobject]@{
        Id = $Id
        Title = $Title
        Reason = $Reason
        RequiresCalibration = $RequiresCalibration
        Impact = $Impact
        Settings = [pscustomobject](Copy-MpcvrSettingsDictionary -Settings $Settings)
    }
}

function New-MpcvrProfileFromRecommendation {
    param(
        [Parameter(Mandatory)]
        [object]$Candidate,
        [Parameter(Mandatory)]
        [object]$Calibration,
        [Parameter(Mandatory)]
        [string]$Name,
        [ValidateSet('Automatic', 'Guided', 'Advanced')]
        [string]$Mode = 'Automatic',
        [object]$SystemProfile
    )

    $sourceFps = [double]$Calibration.Workload.SourceFpsMedian
    $sourceClass = if ($sourceFps -le 30.5) { 'up-to-30' } elseif ($sourceFps -le 60.5) { 'over-30-to-60' } else { 'custom' }
    $settings = $Candidate.Settings
    $profile = [pscustomobject]@{
        profileVersion = 1
        name = $Name
        mode = $Mode.ToLowerInvariant()
        locked = $false
        machineFingerprint = [pscustomobject]@{
            gpuName = $null
            gpuPnpDeviceId = $null
            driverVersion = $null
            displayWidth = $null
            displayHeight = $null
            displayRefreshHz = $null
        }
        conditions = [pscustomobject]@{
            sourceFrameRateClass = $sourceClass
            sourceWidthMax = [int]$Calibration.Workload.SourceWidth
            sourceHeightMax = [int]$Calibration.Workload.SourceHeight
            outputWidth = [int]$Calibration.Workload.OutputWidth
            outputHeight = [int]$Calibration.Workload.OutputHeight
            targetOutputFps = [double]$Calibration.Workload.TargetOutputFpsMedian
        }
        maxine = [pscustomobject]@{
            enabled = ([int64]$settings.MaxineOperation -ne 0)
            operation = switch ([int64]$settings.MaxineOperation) { 1 { 'upscale' } 2 { 'denoise' } 3 { 'deblur' } default { 'disabled' } }
            quality = switch ([int64]$settings.MaxineQuality) { 1 { 'low' } 2 { 'medium' } 3 { 'high' } 4 { 'ultra' } default { 'custom' } }
            scaleLimit = if ([int64]$settings.MaxineScale -eq 0) { 'match-output' } else { [double]$settings.MaxineScale / 100.0 }
            oversampling = ([int64]$settings.MaxineOversample -ne 100)
            denoise = ([int64]$settings.MaxineDenoise -ne 0)
            deblur = ([int64]$settings.MaxineDeblur -ne 0)
            gpuSelection = if ([int64]$settings.MaxineGPU -eq -1) { 'auto' } else { [int64]$settings.MaxineGPU }
        }
        frameInterpolation = [pscustomobject]@{
            enabled = ([int64]$settings.FrameInterpolationMode -ne 0)
            sourceResolutionLimit = switch ([int64]$settings.FrameInterpolationSourceLimit) { 0 { '720p' } 1 { '1080p' } 2 { '1440p' } 3 { '2160p' } default { 'custom' } }
            maxOutputFps = [double]$settings.FrameInterpolationMaxOutput
            runtimePath = $null
            failureFallback = ([int64]$settings.FrameInterpolationFallback -ne 0)
        }
        fallback = [pscustomobject]@{
            strategy = 'ask-user'
            showWarning = $true
            allowOverride = $true
        }
        calibration = [pscustomobject]@{
            capturedAtUtc = [string]$Calibration.CapturedAtUtc
            durationSeconds = [double]$Calibration.DurationSeconds
            maxineMilliseconds = [double]$Calibration.Timing.MaxineMillisecondsMedian
            frameInterpolationMilliseconds = [double]$Calibration.Timing.FrameInterpolationMillisecondsMedian
            totalMilliseconds = [double]$Calibration.Timing.CombinedMillisecondsMedian
            timingHeadroomMilliseconds = [double]$Calibration.Timing.HeadroomMillisecondsP05
            droppedFrames = [int]$Calibration.Frames.Dropped
            failedFrames = [int]$Calibration.Frames.Failed
            pacingStable = [bool]$Calibration.Pacing.Stable
            originalVerdict = [string]$Calibration.Verdict
            recommendationId = [string]$Candidate.Id
            requiresRecalibration = [bool]$Candidate.RequiresCalibration
        }
        rendererSettings = $Candidate.Settings
    }

    if ($null -ne $SystemProfile) {
        $gpu = @($SystemProfile.Gpus | Select-Object -First 1)
        if ($gpu.Count -gt 0) {
            $profile.machineFingerprint.gpuName = [string]$gpu[0].Name
            $profile.machineFingerprint.gpuPnpDeviceId = [string]$gpu[0].PnpDeviceId
            $profile.machineFingerprint.driverVersion = [string]$gpu[0].DriverVersion
            $profile.machineFingerprint.displayWidth = [int]$gpu[0].CurrentWidth
            $profile.machineFingerprint.displayHeight = [int]$gpu[0].CurrentHeight
            $profile.machineFingerprint.displayRefreshHz = [double]$gpu[0].CurrentRefreshHz
        }
    }
    return $profile
}

function Get-MpcvrCalibrationRecommendation {
    param(
        [Parameter(Mandatory)]
        [object]$Calibration,
        [Parameter(Mandatory)]
        [object]$CurrentSettings,
        [ValidateSet('Balanced', 'Smoothness', 'Quality')]
        [string]$Priority = 'Balanced'
    )

    if ($null -eq $Calibration.Workload -or $null -eq $Calibration.Timing -or
        $null -eq $Calibration.Pacing -or $null -eq $Calibration.Frames) {
        throw 'The calibration report is incomplete.'
    }

    $baseSettings = ConvertTo-MpcvrSettingsDictionary -SettingsObject $CurrentSettings.Settings
    Set-MpcvrMeasuredSettings -Settings $baseSettings -Calibration $Calibration
    $candidates = @()
    $verdict = [string]$Calibration.Verdict

    if ($verdict -eq 'Stable') {
        Add-MpcvrRecommendationCandidate `
            -Candidates ([ref]$candidates) `
            -Id 'keep-measured' `
            -Title 'Keep the measured settings' `
            -Reason 'The measured workload met pacing, error, and timing-headroom requirements.' `
            -Settings $baseSettings `
            -RequiresCalibration $false `
            -Impact 'No quality or smoothness reduction'
    }

    $reductionCandidates = [ordered]@{}

    if ([int64]$baseSettings.MaxineOversample -ne 100) {
        $settings = Copy-MpcvrSettingsDictionary -Settings $baseSettings
        $settings.MaxineOversample = 100
        $reductionCandidates['disable-oversampling'] = [pscustomobject]@{
            Title = 'Disable Maxine oversampling'
            Reason = 'Oversampling adds another scaling burden before presentation.'
            Settings = $settings
            Impact = 'Small to moderate quality reduction'
        }
    }

    if ([int64]$baseSettings.MaxineDenoise -ne 0 -or [int64]$baseSettings.MaxineDeblur -ne 0) {
        $settings = Copy-MpcvrSettingsDictionary -Settings $baseSettings
        $settings.MaxineDenoise = 0
        $settings.MaxineDeblur = 0
        $reductionCandidates['disable-extra-filters'] = [pscustomobject]@{
            Title = 'Disable Maxine denoise and deblur'
            Reason = 'Extra Maxine passes consume timing headroom in addition to VSR.'
            Settings = $settings
            Impact = 'Removes denoise/deblur processing'
        }
    }

    if ([int64]$baseSettings.MaxineOperation -ne 0 -and [int64]$baseSettings.MaxineQuality -gt 1) {
        $settings = Copy-MpcvrSettingsDictionary -Settings $baseSettings
        $settings.MaxineQuality = Get-MpcvrLowerQuality -Quality ([int64]$baseSettings.MaxineQuality)
        $reductionCandidates['lower-maxine-quality'] = [pscustomobject]@{
            Title = 'Lower Maxine quality by one level'
            Reason = 'A one-step VSR quality reduction is the least destructive way to recover GPU time.'
            Settings = $settings
            Impact = 'Moderate VSR quality reduction'
        }
    }

    if ([int64]$baseSettings.MaxineOperation -ne 0) {
        $lowerScale = Get-MpcvrLowerScale `
            -Scale ([int64]$baseSettings.MaxineScale) `
            -SourceWidth ([double]$Calibration.Workload.SourceWidth) `
            -OutputWidth ([double]$Calibration.Workload.OutputWidth)
        if ($null -ne $lowerScale) {
            $settings = Copy-MpcvrSettingsDictionary -Settings $baseSettings
            $settings.MaxineScale = [int64]$lowerScale
            $reductionCandidates['lower-maxine-scale'] = [pscustomobject]@{
                Title = ('Cap Maxine scaling at {0:N2}x' -f ([double]$lowerScale / 100.0))
                Reason = 'Reducing the VSR output scale lowers the number of pixels Maxine must process.'
                Settings = $settings
                Impact = 'Reduced upscale resolution'
            }
        }
    }

    if ([int64]$baseSettings.MaxineOperation -ne 0) {
        $settings = Copy-MpcvrSettingsDictionary -Settings $baseSettings
        $settings.MaxineOperation = 0
        $reductionCandidates['disable-maxine'] = [pscustomobject]@{
            Title = 'Disable Maxine for this workload'
            Reason = 'Keeps frame interpolation while removing the Maxine GPU cost.'
            Settings = $settings
            Impact = 'No Maxine enhancement; interpolation remains enabled'
        }
    }

    if ([int64]$baseSettings.FrameInterpolationMode -ne 0) {
        $settings = Copy-MpcvrSettingsDictionary -Settings $baseSettings
        $settings.FrameInterpolationMode = 0
        $reductionCandidates['disable-interpolation'] = [pscustomobject]@{
            Title = 'Disable frame interpolation for this workload'
            Reason = 'Keeps Maxine enhancement while returning playback to the source frame rate.'
            Settings = $settings
            Impact = 'Source frame rate only; Maxine remains enabled'
        }
    }

    $order = switch ($Priority) {
        'Smoothness' { @('disable-oversampling', 'disable-extra-filters', 'lower-maxine-quality', 'lower-maxine-scale', 'disable-maxine', 'disable-interpolation') }
        'Quality' { @('disable-interpolation', 'disable-oversampling', 'disable-extra-filters', 'lower-maxine-quality', 'lower-maxine-scale', 'disable-maxine') }
        default {
            $maxineCost = [double]$Calibration.Timing.MaxineMillisecondsP95
            $frucCost = [double]$Calibration.Timing.FrameInterpolationMillisecondsP95
            $finalOrder = if ($maxineCost -ge $frucCost) { @('disable-maxine', 'disable-interpolation') } else { @('disable-interpolation', 'disable-maxine') }
            @('disable-oversampling', 'disable-extra-filters', 'lower-maxine-quality', 'lower-maxine-scale') + $finalOrder
        }
    }

    foreach ($id in $order) {
        if ($reductionCandidates.Contains($id)) {
            $candidate = $reductionCandidates[$id]
            Add-MpcvrRecommendationCandidate `
                -Candidates ([ref]$candidates) `
                -Id $id `
                -Title $candidate.Title `
                -Reason $candidate.Reason `
                -Settings $candidate.Settings `
                -RequiresCalibration $true `
                -Impact $candidate.Impact
        }
    }

    if ($candidates.Count -eq 0) {
        Add-MpcvrRecommendationCandidate `
            -Candidates ([ref]$candidates) `
            -Id 'no-automatic-change' `
            -Title 'No safe automatic reduction is available' `
            -Reason 'The current report does not expose an enabled stage that can be reduced automatically.' `
            -Settings $baseSettings `
            -RequiresCalibration $true `
            -Impact 'Manual Advanced-mode review required'
    }

    $recommendedIndex = 0
    if ($verdict -eq 'Unstable' -and $candidates.Count -gt 1) {
        if ($Priority -eq 'Smoothness') {
            $match = @($candidates | Where-Object { $_.Id -eq 'disable-maxine' } | Select-Object -First 1)
            if ($match.Count -gt 0) { $recommendedIndex = [array]::IndexOf([object[]]$candidates, $match[0]) }
        }
        elseif ($Priority -eq 'Quality') {
            $match = @($candidates | Where-Object { $_.Id -eq 'disable-interpolation' } | Select-Object -First 1)
            if ($match.Count -gt 0) { $recommendedIndex = [array]::IndexOf([object[]]$candidates, $match[0]) }
        }
        else {
            $preferred = if ([double]$Calibration.Timing.MaxineMillisecondsP95 -ge [double]$Calibration.Timing.FrameInterpolationMillisecondsP95) { 'disable-maxine' } else { 'disable-interpolation' }
            $match = @($candidates | Where-Object { $_.Id -eq $preferred } | Select-Object -First 1)
            if ($match.Count -gt 0) { $recommendedIndex = [array]::IndexOf([object[]]$candidates, $match[0]) }
        }
    }

    for ($i = 0; $i -lt $candidates.Count; $i++) {
        $candidates[$i] | Add-Member -NotePropertyName Rank -NotePropertyValue ($i + 1)
        $candidates[$i] | Add-Member -NotePropertyName Recommended -NotePropertyValue ($i -eq $recommendedIndex)
    }

    return [pscustomobject]@{
        SchemaVersion = 1
        GeneratedAtUtc = [DateTime]::UtcNow.ToString('o')
        CalibrationVerdict = $verdict
        Priority = $Priority
        RecommendedCandidateId = [string]$candidates[$recommendedIndex].Id
        RequiresCalibrationLoop = [bool]$candidates[$recommendedIndex].RequiresCalibration
        Candidates = @($candidates)
    }
}

Export-ModuleMember -Function @(
    'Copy-MpcvrSettingsDictionary',
    'ConvertTo-MpcvrSettingsDictionary',
    'New-MpcvrProfileFromRecommendation',
    'Get-MpcvrCalibrationRecommendation'
)
