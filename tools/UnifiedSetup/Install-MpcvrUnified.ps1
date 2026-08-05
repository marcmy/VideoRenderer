#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RendererArchive,
    [string]$RendererChecksum,
    [string]$MaxineRuntimeArchive,
    [string]$NvOffrucSdkPath,
    [ValidateRange(1, 60)]
    [int]$OfficialDownloadWaitMinutes = 15,
    [switch]$DisableOfficialDownload,
    [switch]$AllowUnverifiedRuntimeFiles,
    [ValidateSet('Automatic', 'Guided', 'Advanced')]
    [string]$Mode = 'Automatic',
    [string]$DataRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup'),
    [switch]$SkipRenderer,
    [switch]$SkipMaxine,
    [switch]$SkipNvOffruc,
    [switch]$ValidateOnly,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 2.0

$commonModule = Join-Path $PSScriptRoot 'MpcvrSetup.Common.psm1'
$transactionModule = Join-Path $PSScriptRoot 'MpcvrSetup.Transaction.psm1'
$toolsRoot = Split-Path -Parent $PSScriptRoot
$maxineInstaller = Join-Path $toolsRoot 'MaxineRuntimeInstaller\Install-MPCVRMaxineRuntime.ps1'
$frucInstaller = Join-Path $toolsRoot 'nvoffruc\Install-NvOFFRUCRuntime.ps1'
$rendererUpdater = Join-Path $toolsRoot 'KLiteMaxineUpdater\Update-KLiteMPCVR.ps1'
$payloadRoot = Join-Path $PSScriptRoot 'payload'

Import-Module -Name $commonModule -Force
Import-Module -Name $transactionModule -Force

function Complete-Run {
    param([int]$ExitCode)

    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

function Add-OptionalPathArgument {
    param(
        [Parameter(Mandatory)]
        [System.Collections.Generic.List[string]]$List,
        [Parameter(Mandatory)]
        [string]$Name,
        [string]$Value
    )

    if (-not [string]::IsNullOrWhiteSpace($Value)) {
        $List.Add($Name)
        $List.Add((ConvertTo-MpcvrCommandLineArgument -Value ([IO.Path]::GetFullPath($Value))))
    }
}

function Resolve-OptionalPayload {
    param(
        [string]$CurrentValue,
        [string]$PayloadName
    )

    if (-not [string]::IsNullOrWhiteSpace($CurrentValue)) {
        return [IO.Path]::GetFullPath($CurrentValue)
    }

    $candidate = Join-Path $payloadRoot $PayloadName
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return $candidate
    }
    return $null
}

function Assert-OptionalFile {
    param(
        [string]$Path,
        [string]$Description
    )

    if (-not [string]::IsNullOrWhiteSpace($Path) -and
        -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description was not found: $Path"
    }
}

function Start-MpcvrElevatedSelf {
    $parts = New-Object 'System.Collections.Generic.List[string]'
    $parts.Add('-NoLogo')
    $parts.Add('-NoProfile')
    $parts.Add('-ExecutionPolicy')
    $parts.Add('Bypass')
    $parts.Add('-File')
    $parts.Add((ConvertTo-MpcvrCommandLineArgument -Value $PSCommandPath))

    Add-OptionalPathArgument -List $parts -Name '-RendererArchive' -Value $RendererArchive
    Add-OptionalPathArgument -List $parts -Name '-RendererChecksum' -Value $RendererChecksum
    Add-OptionalPathArgument -List $parts -Name '-MaxineRuntimeArchive' -Value $MaxineRuntimeArchive
    Add-OptionalPathArgument -List $parts -Name '-NvOffrucSdkPath' -Value $NvOffrucSdkPath
    Add-OptionalPathArgument -List $parts -Name '-DataRoot' -Value $DataRoot
    $parts.Add('-OfficialDownloadWaitMinutes')
    $parts.Add([string]$OfficialDownloadWaitMinutes)
    $parts.Add('-Mode')
    $parts.Add((ConvertTo-MpcvrCommandLineArgument -Value $Mode))

    foreach ($switchName in @(
        'SkipRenderer',
        'SkipMaxine',
        'SkipNvOffruc',
        'DisableOfficialDownload',
        'AllowUnverifiedRuntimeFiles',
        'NoPause'
    )) {
        if ((Get-Variable -Name $switchName -ValueOnly)) {
            $parts.Add("-$switchName")
        }
    }

    try {
        $process = Start-Process `
            -FilePath (Get-MpcvrPowerShellExecutable) `
            -ArgumentList ($parts -join ' ') `
            -Verb RunAs `
            -Wait `
            -PassThru
        exit $process.ExitCode
    }
    catch {
        throw "Administrator elevation was cancelled or failed: $($_.Exception.Message)"
    }
}

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'MPCVR Unified Setup only supports Windows.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

$exitCode = 0
$snapshotRoot = $null
$rollbackAttempted = $false
$rollbackSucceeded = $false
try {
    foreach ($requiredPath in @($commonModule, $transactionModule, $maxineInstaller, $frucInstaller, $rendererUpdater)) {
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
            throw "Unified setup is incomplete. Missing: $requiredPath"
        }
        [void](Test-MpcvrPowerShellScriptSyntax -ScriptPath $requiredPath)
    }

    $RendererArchive = Resolve-OptionalPayload -CurrentValue $RendererArchive -PayloadName 'MpcVideoRenderer-Maxine.zip'
    $RendererChecksum = Resolve-OptionalPayload -CurrentValue $RendererChecksum -PayloadName 'PAYLOAD-SHA256SUMS.txt'
    if ([string]::IsNullOrWhiteSpace($RendererChecksum)) {
        $RendererChecksum = Resolve-OptionalPayload -CurrentValue $null -PayloadName 'SHA256SUMS.txt'
    }
    $MaxineRuntimeArchive = Resolve-OptionalPayload -CurrentValue $MaxineRuntimeArchive -PayloadName 'MPCVR-Maxine-Runtime.zip'
    $NvOffrucSdkPath = Resolve-OptionalPayload -CurrentValue $NvOffrucSdkPath -PayloadName 'Optical_Flow_SDK_5.0.7.zip'

    if ([string]::IsNullOrWhiteSpace($RendererArchive) -xor [string]::IsNullOrWhiteSpace($RendererChecksum)) {
        throw 'RendererArchive and RendererChecksum must be supplied together.'
    }

    Assert-OptionalFile -Path $RendererArchive -Description 'Renderer archive'
    Assert-OptionalFile -Path $RendererChecksum -Description 'Renderer checksum file'
    Assert-OptionalFile -Path $MaxineRuntimeArchive -Description 'Maxine runtime archive'
    if (-not [string]::IsNullOrWhiteSpace($NvOffrucSdkPath) -and
        -not (Test-Path -LiteralPath $NvOffrucSdkPath)) {
        throw "NVIDIA Optical Flow SDK package was not found: $NvOffrucSdkPath"
    }

    if ($ValidateOnly) {
        Invoke-MpcvrPowerShellScript `
            -Name 'Validating the Maxine runtime installer...' `
            -ScriptPath $maxineInstaller `
            -Arguments @('-ValidateOnly', '-SkipEnvironmentUpdate', '-NoPause') `
            -NoNewWindow

        $frucArguments = @('-ValidateOnly', '-NoPause')
        if (-not [string]::IsNullOrWhiteSpace($NvOffrucSdkPath)) {
            $frucArguments += @('-SdkPath', (ConvertTo-MpcvrCommandLineArgument -Value $NvOffrucSdkPath))
        }
        Invoke-MpcvrPowerShellScript `
            -Name 'Validating the NvOFFRUC runtime installer...' `
            -ScriptPath $frucInstaller `
            -Arguments $frucArguments `
            -NoNewWindow

        $rendererArguments = New-Object 'System.Collections.Generic.List[string]'
        $rendererArguments.Add('-ValidateOnly')
        $rendererArguments.Add('-NoPause')
        if (-not [string]::IsNullOrWhiteSpace($RendererArchive)) {
            $rendererArguments.Add('-PackageArchive')
            $rendererArguments.Add((ConvertTo-MpcvrCommandLineArgument -Value $RendererArchive))
            $rendererArguments.Add('-ChecksumFile')
            $rendererArguments.Add((ConvertTo-MpcvrCommandLineArgument -Value $RendererChecksum))
        }
        Invoke-MpcvrPowerShellScript `
            -Name 'Validating the renderer updater...' `
            -ScriptPath $rendererUpdater `
            -Arguments @($rendererArguments) `
            -NoNewWindow

        $profile = Get-MpcvrSystemProfile
        if ($null -eq $profile.Runtimes -or $null -eq $profile.Players) {
            throw 'System inventory validation returned incomplete data.'
        }

        Write-Host
        Write-Host 'Unified transactional setup validation passed.' -ForegroundColor Green
        Complete-Run -ExitCode 0
    }

    if (-not (Test-MpcvrAdministrator)) {
        Start-MpcvrElevatedSelf
    }

    if (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
        throw 'Close MPC-HC before running MPCVR Unified Setup.'
    }

    $profileBefore = Get-MpcvrSystemProfile

    $DataRoot = [IO.Path]::GetFullPath($DataRoot)
    $backupRoot = Join-Path $DataRoot 'backups'
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $backupId = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $snapshotRoot = Join-Path $backupRoot $backupId
    $snapshotItems = @(Get-MpcvrSnapshotItemsFromProfile -Profile $profileBefore)
    [void](New-MpcvrSetupSnapshot -SnapshotRoot $snapshotRoot -Items $snapshotItems)
    [void](Save-MpcvrSystemProfile -Profile $profileBefore -Path (Join-Path $snapshotRoot 'system-before.json'))

    $transaction = [ordered]@{
        SchemaVersion = 1
        Status = 'InProgress'
        Mode = $Mode
        StartedAtUtc = [DateTime]::UtcNow.ToString('o')
        CompletedAtUtc = $null
        BackupPath = $snapshotRoot
        Components = [ordered]@{
            Maxine = -not [bool]$SkipMaxine
            NvOFFRUC = -not [bool]$SkipNvOffruc
            Renderer = -not [bool]$SkipRenderer
        }
        Error = $null
    }
    $transactionPath = Join-Path $snapshotRoot 'transaction.json'
    $transaction | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $transactionPath -Encoding UTF8

    if (-not $SkipMaxine) {
        $arguments = New-Object 'System.Collections.Generic.List[string]'
        if (-not [string]::IsNullOrWhiteSpace($MaxineRuntimeArchive)) {
            $arguments.Add('-RuntimeArchive')
            $arguments.Add((ConvertTo-MpcvrCommandLineArgument -Value $MaxineRuntimeArchive))
        }
        $arguments.Add('-NoPause')
        Invoke-MpcvrPowerShellScript `
            -Name '1/3 Installing NVIDIA Maxine runtime...' `
            -ScriptPath $maxineInstaller `
            -Arguments @($arguments) `
            -NoNewWindow
    }

    if (-not $SkipNvOffruc) {
        $shouldInstallNvOffruc =
            -not [string]::IsNullOrWhiteSpace($NvOffrucSdkPath) -or
            -not [bool]$profileBefore.Runtimes.NvOFFRUC.Installed

        if ($shouldInstallNvOffruc) {
            $frucArguments = New-Object 'System.Collections.Generic.List[string]'
            if (-not [string]::IsNullOrWhiteSpace($NvOffrucSdkPath)) {
                $frucArguments.Add('-SdkPath')
                $frucArguments.Add((ConvertTo-MpcvrCommandLineArgument -Value $NvOffrucSdkPath))
            }
            $frucArguments.Add('-OfficialDownloadWaitMinutes')
            $frucArguments.Add([string]$OfficialDownloadWaitMinutes)
            if ($DisableOfficialDownload) {
                $frucArguments.Add('-DisableOfficialDownload')
            }
            if ($AllowUnverifiedRuntimeFiles) {
                $frucArguments.Add('-AllowUnverifiedRuntimeFiles')
            }
            $frucArguments.Add('-NoPause')

            Invoke-MpcvrPowerShellScript `
                -Name '2/3 Installing NVIDIA NvOFFRUC runtime...' `
                -ScriptPath $frucInstaller `
                -Arguments @($frucArguments) `
                -NoNewWindow
        }
        else {
            Write-Host
            Write-Host '2/3 Preserving the already installed NvOFFRUC runtime.' -ForegroundColor Cyan
        }
    }

    if (-not $SkipRenderer) {
        $arguments = New-Object 'System.Collections.Generic.List[string]'
        if (-not [string]::IsNullOrWhiteSpace($RendererArchive)) {
            $arguments.Add('-PackageArchive')
            $arguments.Add((ConvertTo-MpcvrCommandLineArgument -Value $RendererArchive))
            $arguments.Add('-ChecksumFile')
            $arguments.Add((ConvertTo-MpcvrCommandLineArgument -Value $RendererChecksum))
        }
        $arguments.Add('-NoPause')
        Invoke-MpcvrPowerShellScript `
            -Name '3/3 Installing MPC Video Renderer...' `
            -ScriptPath $rendererUpdater `
            -Arguments @($arguments) `
            -NoNewWindow
    }

    $profileAfter = Get-MpcvrSystemProfile
    if (-not $SkipMaxine -and -not [bool]$profileAfter.Runtimes.Maxine.Installed) {
        throw 'Post-install verification did not find a complete Maxine runtime.'
    }
    if (-not $SkipNvOffruc -and -not [bool]$profileAfter.Runtimes.NvOFFRUC.Installed) {
        throw 'Post-install verification did not find a complete NvOFFRUC runtime.'
    }
    if (-not $SkipRenderer) {
        $installedTargets = @($profileAfter.Players | Where-Object { $_.DirectoryExists })
        if ($installedTargets.Count -eq 0) {
            throw 'No supported MPC-HC/K-Lite renderer target was detected after installation.'
        }
        $missingRenderers = @($installedTargets | Where-Object { -not $_.RendererExists })
        if ($missingRenderers.Count -gt 0) {
            throw ('Renderer verification failed for: {0}' -f (($missingRenderers | ForEach-Object { $_.Name }) -join ', '))
        }
    }

    [void](Save-MpcvrSystemProfile -Profile $profileAfter -Path (Join-Path $snapshotRoot 'system-after.json'))
    $transaction['Status'] = 'Succeeded'
    $transaction['CompletedAtUtc'] = [DateTime]::UtcNow.ToString('o')
    $transaction | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $transactionPath -Encoding UTF8

    New-Item -ItemType Directory -Path $DataRoot -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $DataRoot 'latest-backup.txt') -Value $snapshotRoot -Encoding UTF8

    Write-Host
    Write-Host 'MPCVR Unified Setup completed successfully.' -ForegroundColor Green
    Write-Host "Configuration mode: $Mode"
    Write-Host "Rollback snapshot: $snapshotRoot"
    Write-Host 'Restart MPC-HC, play a video, and press Ctrl+J to verify both NVIDIA runtimes.'
}
catch {
    $exitCode = 1
    $message = $_.Exception.Message
    Write-Host
    Write-Host "Unified setup failed: $message" -ForegroundColor Red

    if (-not [string]::IsNullOrWhiteSpace($snapshotRoot) -and
        (Test-Path -LiteralPath (Join-Path $snapshotRoot 'manifest.json') -PathType Leaf)) {
        $rollbackAttempted = $true
        try {
            Write-Host 'Restoring the pre-install renderer and runtime state...' -ForegroundColor Yellow
            [void](Restore-MpcvrSetupSnapshot -SnapshotRoot $snapshotRoot)
            $rollbackSucceeded = $true
            Write-Host 'Automatic rollback completed.' -ForegroundColor Green
        }
        catch {
            Write-Host "Automatic rollback also failed: $($_.Exception.Message)" -ForegroundColor Red
        }

        $transactionPath = Join-Path $snapshotRoot 'transaction.json'
        if (Test-Path -LiteralPath $transactionPath -PathType Leaf) {
            $transaction = Get-Content -LiteralPath $transactionPath -Raw | ConvertFrom-Json
            if ($rollbackSucceeded) {
                $transaction.Status = 'FailedRolledBack'
            }
            elseif ($rollbackAttempted) {
                $transaction.Status = 'FailedRollbackFailed'
            }
            else {
                $transaction.Status = 'Failed'
            }
            $transaction.CompletedAtUtc = [DateTime]::UtcNow.ToString('o')
            $transaction.Error = $message
            $transaction | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $transactionPath -Encoding UTF8
        }
    }

    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}

Complete-Run -ExitCode $exitCode





