#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$NvOffrucSdkPath,
    [string]$MediaPath,
    [ValidateSet('Automatic', 'Guided', 'Advanced')]
    [string]$Mode = 'Automatic',
    [ValidateSet('Balanced', 'Smoothness', 'Quality')]
    [string]$Priority = 'Balanced',
    [ValidateRange(1, 12)]
    [int]$MaxAttempts = 6,
    [ValidateRange(3, 300)]
    [int]$DurationSeconds = 12,
    [string]$ProfileName,
    [switch]$LockProfile,
    [switch]$SkipInstall,
    [switch]$SkipAutoTune,
    [switch]$ValidateOnly,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$installerScript = Join-Path $PSScriptRoot 'Install-MpcvrUnified.ps1'
$autoTuneScript = Join-Path $PSScriptRoot 'Invoke-MpcvrAutoTune.ps1'
foreach ($path in @($installerScript, $autoTuneScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "First-run dependency is missing: $path"
    }
}

function Complete-Run {
    param([int]$ExitCode)
    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

function Get-PowerShellExecutable {
    $pwsh = Get-Command 'pwsh.exe' -ErrorAction SilentlyContinue
    if ($null -ne $pwsh) { return $pwsh.Source }
    $powershell = Get-Command 'powershell.exe' -ErrorAction SilentlyContinue
    if ($null -ne $powershell) { return $powershell.Source }
    throw 'Neither PowerShell 7 nor Windows PowerShell 5.1 was found.'
}

function Quote-Argument {
    param([string]$Value)
    return '"{0}"' -f $Value.Replace('"', '\"')
}

function Invoke-ScriptStep {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [Parameter(Mandatory)]
        [string]$ScriptPath,
        [string[]]$Arguments = @()
    )

    Write-Host
    Write-Host $Name -ForegroundColor Cyan
    $parts = @(
        '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (Quote-Argument -Value $ScriptPath)
    ) + @($Arguments)
    $process = Start-Process `
        -FilePath (Get-PowerShellExecutable) `
        -ArgumentList ($parts -join ' ') `
        -Wait `
        -PassThru
    return $process.ExitCode
}

if ($ValidateOnly) {
    $installCode = Invoke-ScriptStep `
        -Name 'Validating unified installer...' `
        -ScriptPath $installerScript `
        -Arguments @('-ValidateOnly', '-NoPause')
    if ($installCode -ne 0) {
        throw "Unified installer validation failed with exit code $installCode."
    }
    $autoCode = Invoke-ScriptStep `
        -Name 'Validating automatic calibration...' `
        -ScriptPath $autoTuneScript `
        -Arguments @('-ValidateOnly')
    if ($autoCode -ne 0) {
        throw "Automatic calibration validation failed with exit code $autoCode."
    }
    Write-Host 'MPCVR first-run sequence validation passed.' -ForegroundColor Green
    Complete-Run -ExitCode 0
}

$exitCode = 0
try {
    if (-not $SkipInstall) {
        $installArguments = @('-Mode', $Mode, '-NoPause')
        if (-not [string]::IsNullOrWhiteSpace($NvOffrucSdkPath)) {
            $resolvedSdk = (Resolve-Path -LiteralPath $NvOffrucSdkPath).Path
            $installArguments += @('-NvOffrucSdkPath', (Quote-Argument -Value $resolvedSdk))
        }
        $installCode = Invoke-ScriptStep `
            -Name 'Installing MPCVR, Maxine, and NvOFFRUC...' `
            -ScriptPath $installerScript `
            -Arguments $installArguments
        if ($installCode -ne 0) {
            throw "Unified installation failed with exit code $installCode."
        }
    }

    if (-not $SkipAutoTune) {
        if ([string]::IsNullOrWhiteSpace($MediaPath)) {
            throw 'MediaPath is required unless SkipAutoTune is selected.'
        }
        $resolvedMedia = (Resolve-Path -LiteralPath $MediaPath).Path
        $autoArguments = @(
            '-MediaPath', (Quote-Argument -Value $resolvedMedia),
            '-Priority', $Priority,
            '-MaxAttempts', $MaxAttempts,
            '-DurationSeconds', $DurationSeconds
        )
        if (-not [string]::IsNullOrWhiteSpace($ProfileName)) {
            $autoArguments += @('-ProfileName', (Quote-Argument -Value $ProfileName))
        }
        if ($LockProfile) {
            $autoArguments += '-LockProfile'
        }
        if ($NoPause) {
            $autoArguments += '-Json'
        }

        $autoCode = Invoke-ScriptStep `
            -Name 'Calibrating and selecting stable renderer settings...' `
            -ScriptPath $autoTuneScript `
            -Arguments $autoArguments
        if ($autoCode -eq 2) {
            throw 'Automatic calibration tested every allowed candidate but did not find a passing configuration. Original renderer settings were restored.'
        }
        if ($autoCode -ne 0) {
            throw "Automatic calibration failed with exit code $autoCode."
        }
    }

    Write-Host
    Write-Host 'MPCVR first-run setup completed successfully.' -ForegroundColor Green
}
catch {
    $exitCode = 1
    Write-Host
    Write-Host "First-run setup failed: $($_.Exception.Message)" -ForegroundColor Red
}

Complete-Run -ExitCode $exitCode
