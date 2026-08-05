#requires -Version 5.1

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$profilesModule = Join-Path $PSScriptRoot 'MpcvrSetup.Profiles.psm1'
$settingsModule = Join-Path $PSScriptRoot 'MpcvrSetup.RendererSettings.psm1'
$recommendationsModule = Join-Path $PSScriptRoot 'MpcvrSetup.Recommendations.psm1'
$applyScript = Join-Path $PSScriptRoot 'Set-MpcvrRendererProfile.ps1'
$recommendScript = Join-Path $PSScriptRoot 'New-MpcvrRecommendation.ps1'
foreach ($path in @($profilesModule, $settingsModule, $recommendationsModule, $applyScript, $recommendScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Renderer recommendation dependency is missing: $path"
    }
}

Import-Module -Name $profilesModule -Force
Import-Module -Name $settingsModule -Force
Import-Module -Name $recommendationsModule -Force

function Assert-Equal {
    param($Actual, $Expected, [string]$Message)
    if ($Actual -ne $Expected) {
        throw "$Message Expected '$Expected' but found '$Actual'."
    }
}

function New-SyntheticCalibration {
    param(
        [ValidateSet('Stable', 'Marginal', 'Unstable')]
        [string]$Verdict,
        [double]$MaxineP95 = 6.0,
        [double]$FrucP95 = 8.0,
        [double]$HeadroomP05 = 2.0,
        [bool]$PacingStable = $true
    )

    return [pscustomobject]@{
        SchemaVersion = 1
        CapturedAtUtc = [DateTime]::UtcNow.ToString('o')
        DurationSeconds = 12
        Verdict = $Verdict
        Workload = [pscustomobject]@{
            SourceWidth = 1920
            SourceHeight = 1080
            OutputWidth = 2560
            OutputHeight = 1440
            SourceFpsMedian = 59.94
            TargetOutputFpsMedian = 119.88
            MaxineEnabled = $true
            MaxineActive = $true
            FrameInterpolationEnabled = $true
            FrameInterpolationActive = $true
            CombinedActive = $true
        }
        Timing = [pscustomobject]@{
            SourceFrameBudgetMillisecondsMedian = 16.68
            MaxineMillisecondsMedian = $MaxineP95 - 0.5
            MaxineMillisecondsP95 = $MaxineP95
            FrameInterpolationMillisecondsMedian = $FrucP95 - 0.5
            FrameInterpolationMillisecondsP95 = $FrucP95
            CombinedMillisecondsMedian = $MaxineP95 + $FrucP95 - 1.0
            CombinedMillisecondsP95 = $MaxineP95 + $FrucP95
            HeadroomMillisecondsMedian = $HeadroomP05 + 0.5
            HeadroomMillisecondsP05 = $HeadroomP05
        }
        Pacing = [pscustomobject]@{
            MeasuredDrawFpsMedian = if ($PacingStable) { 119.88 } else { 102.0 }
            MeasuredDrawFpsP10 = if ($PacingStable) { 118.5 } else { 90.0 }
            Stable = $PacingStable
        }
        Frames = [pscustomobject]@{
            SourceFramesObserved = 720
            Dropped = if ($Verdict -eq 'Unstable') { 2 } else { 0 }
            Skipped = 0
            Failed = 0
        }
        RendererSettings = [pscustomobject]@{
            MaxineOperation = 1
            MaxineQuality = 3
            MaxineScale = 200
            MaxineOversample = 133
            FrameInterpolationMode = 1
            FrameInterpolationSourceLimit = 1
            FrameInterpolationMaxOutput = 120
        }
    }
}

$token = [guid]::NewGuid().ToString('N')
$registryPath = "HKCU:\Software\MPCVRUnifiedSetupTests\$token"
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("MPCVR-RendererSettings-Test-$token")
try {
    New-Item -Path $registryPath -Force | Out-Null
    New-ItemProperty -LiteralPath $registryPath -Name 'MaxineQuality' -Value 4 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -LiteralPath $registryPath -Name 'KeepMe' -Value 77 -PropertyType DWord -Force | Out-Null
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

    $definitions = @(Get-MpcvrRendererSettingDefinitions)
    Assert-Equal $definitions.Count 16 'Unexpected renderer setting definition count.'

    $semantic = New-MpcvrProfile -Name 'Semantic Test' -Mode Guided -SourceFrameRateClass 'over-30-to-60'
    $semantic.maxine.enabled = $true
    $semantic.maxine.operation = 'upscale'
    $semantic.maxine.quality = 'medium'
    $semantic.maxine.scaleLimit = 2.0
    $semantic.maxine.oversampling = $false
    $semantic.maxine.denoise = $false
    $semantic.maxine.deblur = $false
    $semantic.frameInterpolation.enabled = $true
    $semantic.frameInterpolation.sourceResolutionLimit = '1080p'
    $semantic.frameInterpolation.maxOutputFps = 120

    $desired = ConvertTo-MpcvrRendererSettings -Profile $semantic
    Assert-Equal $desired.MaxineOperation 1 'Semantic Maxine operation mapping failed.'
    Assert-Equal $desired.MaxineQuality 2 'Semantic Maxine quality mapping failed.'
    Assert-Equal $desired.MaxineScale 200 'Semantic Maxine scale mapping failed.'
    Assert-Equal $desired.MaxineOversample 100 'Semantic oversampling mapping failed.'
    Assert-Equal $desired.FrameInterpolationMode 1 'Semantic FRUC mode mapping failed.'
    Assert-Equal $desired.FrameInterpolationMaxOutput 120 'Semantic FRUC output mapping failed.'

    $dryRun = Set-MpcvrRendererSettings `
        -Settings $desired `
        -RegistryPath $registryPath `
        -DryRun `
        -AllowPlayerRunning
    if ($dryRun.Applied -or @($dryRun.Changes).Count -eq 0) {
        throw 'Renderer settings dry-run did not report pending changes.'
    }
    if ((Get-ItemProperty -LiteralPath $registryPath -Name 'MaxineOperation' -ErrorAction SilentlyContinue)) {
        throw 'Renderer settings dry-run modified the registry.'
    }

    $backupPath = Join-Path $tempRoot 'settings-before.json'
    $applied = Set-MpcvrRendererSettings `
        -Settings $desired `
        -RegistryPath $registryPath `
        -BackupPath $backupPath `
        -AllowPlayerRunning
    if (-not $applied.Applied -or -not (Test-Path -LiteralPath $backupPath -PathType Leaf)) {
        throw 'Renderer settings apply did not create a verified backup.'
    }
    $after = Get-MpcvrRendererSettings -RegistryPath $registryPath
    Assert-Equal $after.Settings.MaxineOperation 1 'Applied Maxine operation did not verify.'
    Assert-Equal $after.Settings.MaxineQuality 2 'Applied Maxine quality did not verify.'
    Assert-Equal $after.Settings.FrameInterpolationMode 1 'Applied FRUC mode did not verify.'
    Assert-Equal (Get-ItemPropertyValue -LiteralPath $registryPath -Name 'KeepMe') 77 'Unknown registry values were modified.'

    New-ItemProperty -LiteralPath $registryPath -Name 'MaxineQuality' -Value 1 -PropertyType DWord -Force | Out-Null
    [void](Restore-MpcvrRendererSettingsBackup `
        -BackupPath $backupPath `
        -RegistryPath $registryPath `
        -AllowPlayerRunning)
    $restored = Get-MpcvrRendererSettings -RegistryPath $registryPath
    Assert-Equal $restored.Settings.MaxineQuality 4 'Settings restore did not restore the original DWORD.'
    if ((Get-Item -LiteralPath $registryPath).GetValueNames() -contains 'MaxineOperation') {
        throw 'Settings restore did not remove a value that was absent before apply.'
    }
    Assert-Equal (Get-ItemPropertyValue -LiteralPath $registryPath -Name 'KeepMe') 77 'Settings restore removed an unknown value.'

    $exact = New-MpcvrProfile -Name 'Exact Advanced Test' -Mode Advanced -SourceFrameRateClass custom -Locked
    $exact | Add-Member -NotePropertyName rendererSettings -NotePropertyValue ([pscustomobject]@{
        MaxineOperation = 1
        MaxineSourceMode = 2
        MaxineQuality = 4
        MaxineScale = 300
        MaxineOversample = 150
        MaxineSourceLimit = 4
        MaxineDenoise = 2
        MaxineDeblur = 1
        MaxinePipeline = 5
        MaxineGPU = -1
        MaxineAutoBitrate = 35
        FrameInterpolationMode = 1
        FrameInterpolationSourceLimit = 3
        FrameInterpolationMaxOutput = 240
        FrameInterpolationGPU = -1
        FrameInterpolationFallback = 1
    })
    $exactSettings = ConvertTo-MpcvrRendererSettings -Profile $exact
    Assert-Equal $exactSettings.MaxineGPU -1 'Exact Advanced GPU mapping failed.'
    Assert-Equal $exactSettings.MaxinePipeline 5 'Exact Advanced pipeline mapping failed.'
    Assert-Equal $exactSettings.FrameInterpolationMaxOutput 240 'Exact Advanced FRUC mapping failed.'

    [void](Set-MpcvrRendererSettings `
        -Settings $exactSettings `
        -RegistryPath $registryPath `
        -BackupPath (Join-Path $tempRoot 'exact-backup.json') `
        -AllowPlayerRunning)
    $current = Get-MpcvrRendererSettings -RegistryPath $registryPath

    $stable = Get-MpcvrCalibrationRecommendation `
        -Calibration (New-SyntheticCalibration -Verdict Stable) `
        -CurrentSettings $current `
        -Priority Balanced
    Assert-Equal $stable.RecommendedCandidateId 'keep-measured' 'Stable calibration recommendation was not preserved.'
    if ($stable.RequiresCalibrationLoop) {
        throw 'Stable measured settings were incorrectly marked for recalibration.'
    }

    $marginal = Get-MpcvrCalibrationRecommendation `
        -Calibration (New-SyntheticCalibration -Verdict Marginal -HeadroomP05 0.25) `
        -CurrentSettings $current `
        -Priority Balanced
    Assert-Equal $marginal.RecommendedCandidateId 'disable-oversampling' 'Marginal Balanced ordering is not conservative.'
    if (-not $marginal.RequiresCalibrationLoop) {
        throw 'A changed recommendation was not marked for recalibration.'
    }

    $unstableSmoothness = Get-MpcvrCalibrationRecommendation `
        -Calibration (New-SyntheticCalibration -Verdict Unstable -PacingStable $false -MaxineP95 9 -FrucP95 7 -HeadroomP05 -2) `
        -CurrentSettings $current `
        -Priority Smoothness
    Assert-Equal $unstableSmoothness.RecommendedCandidateId 'disable-maxine' 'Smoothness priority did not preserve interpolation.'

    $unstableQuality = Get-MpcvrCalibrationRecommendation `
        -Calibration (New-SyntheticCalibration -Verdict Unstable -PacingStable $false -MaxineP95 5 -FrucP95 10 -HeadroomP05 -2) `
        -CurrentSettings $current `
        -Priority Quality
    Assert-Equal $unstableQuality.RecommendedCandidateId 'disable-interpolation' 'Quality priority did not preserve Maxine.'

    $systemProfile = [pscustomobject]@{
        Gpus = @([pscustomobject]@{
            Name = 'Synthetic GPU'
            PnpDeviceId = 'PCI\\SYNTHETIC'
            DriverVersion = '1.2.3'
            CurrentWidth = 2560
            CurrentHeight = 1440
            CurrentRefreshHz = 144.0
        })
    }
    $recommendedCandidate = @($marginal.Candidates | Where-Object { $_.Recommended } | Select-Object -First 1)[0]
    $generatedProfile = New-MpcvrProfileFromRecommendation `
        -Candidate $recommendedCandidate `
        -Calibration (New-SyntheticCalibration -Verdict Marginal -HeadroomP05 0.25) `
        -Name 'Generated Recommendation' `
        -Mode Automatic `
        -SystemProfile $systemProfile
    $validation = Test-MpcvrProfile -Profile $generatedProfile
    if (-not $validation.Valid -or $generatedProfile.machineFingerprint.gpuName -ne 'Synthetic GPU' -or
        -not [bool]$generatedProfile.calibration.requiresRecalibration) {
        throw 'Generated recommendation profile did not preserve validation, fingerprint, or recalibration state.'
    }

    $profileRoot = Join-Path $tempRoot 'profiles'
    $generatedPath = Save-MpcvrProfile -Profile $generatedProfile -ProfileRoot $profileRoot
    $previewJson = & $applyScript `
        -Action Preview `
        -ProfilePath $generatedPath `
        -RegistryPath $registryPath `
        -AllowPlayerRunning `
        -Json
    $preview = $previewJson | Out-String | ConvertFrom-Json
    if ($preview.Action -ne 'Preview' -or $null -eq $preview.Changes) {
        throw 'Renderer profile preview entry point failed.'
    }

    $calibrationPath = Join-Path $tempRoot 'calibration.json'
    New-SyntheticCalibration -Verdict Marginal -HeadroomP05 0.25 |
        ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $calibrationPath -Encoding UTF8
    $recommendationPath = Join-Path $tempRoot 'recommendation.json'
    & $recommendScript `
        -CalibrationPath $calibrationPath `
        -Priority Balanced `
        -RegistryPath $registryPath `
        -OutputPath $recommendationPath `
        -Json | Out-Null
    if (-not (Test-Path -LiteralPath $recommendationPath -PathType Leaf)) {
        throw 'Recommendation command did not create its report.'
    }
    $commandReport = Get-Content -LiteralPath $recommendationPath -Raw | ConvertFrom-Json
    Assert-Equal $commandReport.Recommendation.RecommendedCandidateId 'disable-oversampling' 'Recommendation command selected an unexpected candidate.'
}
finally {
    Remove-Item -LiteralPath $registryPath -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'Renderer settings transactions and calibration recommendations validated successfully.' -ForegroundColor Green
