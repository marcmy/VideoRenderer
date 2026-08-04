#requires -Version 5.1

[CmdletBinding()]
param(
    [switch]$NoPause,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$powerShellExecutable = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$payloadRoot = Join-Path $PSScriptRoot 'payload'
$runtimeInstaller = Join-Path $payloadRoot 'Install-MPCVRMaxineRuntime.ps1'
$updaterInstaller = Join-Path $payloadRoot 'Install-KLiteMPCVRUpdater.ps1'
$rendererUpdater = Join-Path $payloadRoot 'Update-KLiteMPCVR.ps1'
$runtimeArchive = Join-Path $payloadRoot 'MPCVR-Maxine-Runtime.zip'
$runtimeChecksum = Join-Path $payloadRoot 'MPCVR-Maxine-Runtime.zip.sha256'
$rendererArchive = Join-Path $payloadRoot 'MpcVideoRenderer-Maxine.zip'
$payloadChecksums = Join-Path $payloadRoot 'PAYLOAD-SHA256SUMS.txt'

$requiredPayload = @(
    $runtimeInstaller,
    $updaterInstaller,
    $rendererUpdater,
    $runtimeArchive,
    $runtimeChecksum,
    $rendererArchive,
    $payloadChecksums
)

function Complete-Run {
    param([int]$ExitCode)

    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

function Quote-ProcessArgument {
    param(
        [Parameter(Mandatory)]
        [string]$Value
    )

    return '"{0}"' -f $Value.Replace('"', '\"')
}

function Get-ExpectedHash {
    param(
        [Parameter(Mandatory)]
        [string]$ChecksumPath,
        [Parameter(Mandatory)]
        [string]$FileName
    )

    $text = Get-Content -LiteralPath $ChecksumPath -Raw
    foreach ($line in ($text -split "`r?`n")) {
        $match = [regex]::Match($line, '^\s*([a-fA-F0-9]{64})\s+\*?(.+?)\s*$')
        if ($match.Success -and $match.Groups[2].Value -ieq $FileName) {
            return $match.Groups[1].Value.ToLowerInvariant()
        }
    }

    $singleHash = [regex]::Match($text, '(?i)\b[a-f0-9]{64}\b')
    if ($singleHash.Success) {
        return $singleHash.Value.ToLowerInvariant()
    }

    throw "No valid SHA-256 entry for $FileName was found in $ChecksumPath."
}

function Test-FileHash {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,
        [Parameter(Mandatory)]
        [string]$ChecksumPath,
        [Parameter(Mandatory)]
        [string]$ChecksumFileName
    )

    $expectedHash = Get-ExpectedHash -ChecksumPath $ChecksumPath -FileName $ChecksumFileName
    $actualHash = (Get-FileHash -LiteralPath $FilePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "SHA-256 verification failed for $ChecksumFileName. Expected $expectedHash but found $actualHash."
    }
    return $actualHash
}

function Invoke-SetupStep {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [Parameter(Mandatory)]
        [string]$ScriptPath,
        [string]$AdditionalArguments = ''
    )

    Write-Host
    Write-Host $Name -ForegroundColor Cyan
    $arguments = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File {0}' -f (Quote-ProcessArgument -Value $ScriptPath)
    if (-not [string]::IsNullOrWhiteSpace($AdditionalArguments)) {
        $arguments += " $AdditionalArguments"
    }

    $process = Start-Process -FilePath $powerShellExecutable -ArgumentList $arguments -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "$Name failed with exit code $($process.ExitCode)."
    }
}

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'MPCVR Maxine Setup only supports Windows.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if (-not (Test-Path -LiteralPath $powerShellExecutable -PathType Leaf)) {
    Write-Host "Windows PowerShell 5.1 was not found at $powerShellExecutable." -ForegroundColor Red
    Complete-Run -ExitCode 1
}

$exitCode = 0
try {
    foreach ($path in $requiredPayload) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "The setup package is incomplete. Missing: $path"
        }
    }

    $runtimeHash = Test-FileHash `
        -FilePath $runtimeArchive `
        -ChecksumPath $runtimeChecksum `
        -ChecksumFileName ([IO.Path]::GetFileName($runtimeArchive))
    $rendererHash = Test-FileHash `
        -FilePath $rendererArchive `
        -ChecksumPath $payloadChecksums `
        -ChecksumFileName ([IO.Path]::GetFileName($rendererArchive))

    Write-Host 'Verified setup payloads:' -ForegroundColor Green
    Write-Host "  Maxine runtime: $runtimeHash"
    Write-Host "  MPC Video Renderer: $rendererHash"

    if ($ValidateOnly) {
        $validationArguments = '-PackageArchive {0} -ChecksumFile {1} -ValidateOnly -NoPause' -f `
            (Quote-ProcessArgument -Value $rendererArchive), `
            (Quote-ProcessArgument -Value $payloadChecksums)
        Invoke-SetupStep `
            -Name 'Validating the renderer updater...' `
            -ScriptPath $rendererUpdater `
            -AdditionalArguments $validationArguments

        Write-Host
        Write-Host 'Unified Maxine setup validation passed.' -ForegroundColor Green
        Complete-Run -ExitCode 0
    }

    if (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
        throw 'Close MPC-HC before running MPCVR Maxine Setup.'
    }

    $runtimeArguments = '-RuntimeArchive {0} -NoPause' -f (Quote-ProcessArgument -Value $runtimeArchive)
    Invoke-SetupStep `
        -Name '1/3 Installing the NVIDIA Maxine runtime...' `
        -ScriptPath $runtimeInstaller `
        -AdditionalArguments $runtimeArguments

    Invoke-SetupStep `
        -Name '2/3 Installing the K-Lite restore shortcut...' `
        -ScriptPath $updaterInstaller `
        -AdditionalArguments '-NoPause'

    $rendererArguments = '-PackageArchive {0} -ChecksumFile {1} -NoPause' -f `
        (Quote-ProcessArgument -Value $rendererArchive), `
        (Quote-ProcessArgument -Value $payloadChecksums)
    Invoke-SetupStep `
        -Name '3/3 Installing the custom MPC Video Renderer into K-Lite...' `
        -ScriptPath $rendererUpdater `
        -AdditionalArguments $rendererArguments

    Write-Host
    Write-Host 'MPCVR Maxine Setup completed successfully.' -ForegroundColor Green
    Write-Host 'The Maxine runtime, custom renderer, and desktop restore shortcut are ready.'
    Write-Host 'Open MPC-HC, play a video, and press Ctrl+J to confirm the Maxine runtime path.'
}
catch {
    $exitCode = 1
    Write-Host
    Write-Host "Setup failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}

Complete-Run -ExitCode $exitCode
