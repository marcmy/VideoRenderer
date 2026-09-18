#requires -Version 5.1

[CmdletBinding()]
param(
    [switch]$NoPause,
    [switch]$ValidateOnly,
    [string]$GpuInventoryJson
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$powerShellExecutable = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$payloadRoot = Join-Path $PSScriptRoot 'payload'
$payloadChecksums = Join-Path $payloadRoot 'PAYLOAD-SHA256SUMS.txt'

$maxineRuntimeInstaller = Join-Path $payloadRoot 'Install-MPCVRMaxineRuntime.ps1'
$rifeRuntimeInstaller = Join-Path $payloadRoot 'Install-MPCVRRifeRuntime.ps1'
$rifePreflight = Join-Path $payloadRoot 'Test-MPCVRRifePreflight.ps1'
$updaterInstaller = Join-Path $payloadRoot 'Install-KLiteMPCVRUpdater.ps1'
$rendererUpdater = Join-Path $payloadRoot 'Update-KLiteMPCVR.ps1'

$rendererArchive = Join-Path $payloadRoot 'MpcVideoRenderer-Maxine-RIFE.zip'
$maxineRuntimeArchive = Join-Path $payloadRoot 'MPCVR-Maxine-Runtime.zip'
$rifeModelArchive = Join-Path $payloadRoot 'MPCVR-RIFE-Model-v4.6.zip'
$rifeRuntimeManifest = Join-Path $payloadRoot 'RIFE-runtime-manifest.json'

$rifeRuntimeArchiveNames = @(
    'MPCVR-RIFE-Common.zip',
    'MPCVR-RIFE-sm75.zip',
    'MPCVR-RIFE-sm86.zip',
    'MPCVR-RIFE-sm89.zip',
    'MPCVR-RIFE-sm120.zip'
)

$requiredPayloadNames = @(
    'Install-MPCVRMaxineRuntime.ps1',
    'Install-MPCVRRifeRuntime.ps1',
    'Test-MPCVRRifePreflight.ps1',
    'RifeRuntimeManifest.psm1',
    'Install-KLiteMPCVRUpdater.ps1',
    'Update-KLiteMPCVR.ps1',
    'KLiteRendererInstall.psm1',
    'MpcVideoRenderer-Maxine-RIFE.zip',
    'MPCVR-Maxine-Runtime.zip',
    'MPCVR-RIFE-Common.zip',
    'MPCVR-RIFE-sm75.zip',
    'MPCVR-RIFE-sm86.zip',
    'MPCVR-RIFE-sm89.zip',
    'MPCVR-RIFE-sm120.zip',
    'MPCVR-RIFE-Model-v4.6.zip',
    'RIFE-runtime-manifest.json',
    'PAYLOAD-SHA256SUMS.txt'
)

function Complete-Run {
    param([int]$ExitCode)

    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

function Get-ExpectedHash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ChecksumPath,
        [Parameter(Mandatory = $true)]
        [string]$FileName
    )

    $text = Get-Content -LiteralPath $ChecksumPath -Raw
    $hashes = @()
    foreach ($line in ($text -split "`r?`n")) {
        $match = [regex]::Match($line, '^\s*([a-fA-F0-9]{64})\s+\*?(.+?)\s*$')
        if (-not $match.Success) {
            continue
        }

        $hash = $match.Groups[1].Value.ToLowerInvariant()
        $name = $match.Groups[2].Value.Replace('\', '/')
        $hashes += $hash
        if ($name -ieq $FileName.Replace('\', '/')) {
            return $hash
        }
    }

    if ($hashes.Count -eq 1) {
        return $hashes[0]
    }

    throw "No valid SHA-256 entry for $FileName was found in $ChecksumPath."
}

function Test-FileHash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string]$ChecksumPath,
        [Parameter(Mandatory = $true)]
        [string]$ChecksumFileName
    )

    $expectedHash = Get-ExpectedHash -ChecksumPath $ChecksumPath -FileName $ChecksumFileName
    $actualHash = (Get-FileHash -LiteralPath $FilePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "SHA-256 verification failed for $ChecksumFileName. Expected $expectedHash but found $actualHash."
    }
    return $actualHash
}

function Resolve-PayloadPath {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    $root = [IO.Path]::GetFullPath($payloadRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $relative = $RelativePath.Replace('/', [IO.Path]::DirectorySeparatorChar).Replace('\', [IO.Path]::DirectorySeparatorChar)
    $candidate = [IO.Path]::GetFullPath((Join-Path $payloadRoot $relative))
    if (-not $candidate.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Payload checksum path escapes the payload directory: $RelativePath"
    }
    return $candidate
}

function Assert-PayloadHashes {
    foreach ($name in $requiredPayloadNames) {
        $path = Join-Path $payloadRoot $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "The setup package is incomplete. Missing: $path"
        }
    }

    foreach ($name in ($requiredPayloadNames | Where-Object { $_ -ne 'PAYLOAD-SHA256SUMS.txt' })) {
        [void](Test-FileHash -FilePath (Join-Path $payloadRoot $name) -ChecksumPath $payloadChecksums -ChecksumFileName $name)
    }

    $seen = @{}
    foreach ($line in ((Get-Content -LiteralPath $payloadChecksums -Raw) -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        $match = [regex]::Match($line, '^\s*([a-fA-F0-9]{64})\s+\*?(.+?)\s*$')
        if (-not $match.Success) {
            throw "Invalid PAYLOAD-SHA256SUMS.txt entry: $line"
        }

        $name = $match.Groups[2].Value.Replace('\', '/')
        if ($seen.ContainsKey($name.ToLowerInvariant())) {
            throw "Duplicate PAYLOAD-SHA256SUMS.txt entry: $name"
        }
        $seen[$name.ToLowerInvariant()] = $true

        $path = Resolve-PayloadPath -RelativePath $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "PAYLOAD-SHA256SUMS.txt declares a missing file: $name"
        }

        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        $expectedHash = $match.Groups[1].Value.ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "SHA-256 verification failed for $name. Expected $expectedHash but found $actualHash."
        }
    }
}

function Invoke-SetupStep {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,
        [hashtable]$Parameters = @{}
    )

    Write-Host
    Write-Host $Name -ForegroundColor Cyan
    $parameterJson = ConvertTo-Json -InputObject $Parameters -Compress
    $parameterBase64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($parameterJson))
    $launcher = @'
$ErrorActionPreference = 'Stop'
try {
    $parameterJson = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($env:MPCVR_SETUP_STEP_PARAMS_B64))
    $parameterObject = $parameterJson | ConvertFrom-Json
    $stepParameters = @{}
    foreach ($property in $parameterObject.PSObject.Properties) {
        $stepParameters[$property.Name] = $property.Value
    }
    & $env:MPCVR_SETUP_STEP_SCRIPT @stepParameters *>&1 | ForEach-Object { $_.ToString() }
    exit 0
}
catch {
    Write-Error $_
    exit 1
}
'@
    $previousScript = $env:MPCVR_SETUP_STEP_SCRIPT
    $previousParameters = $env:MPCVR_SETUP_STEP_PARAMS_B64
    try {
        $env:MPCVR_SETUP_STEP_SCRIPT = $ScriptPath
        $env:MPCVR_SETUP_STEP_PARAMS_B64 = $parameterBase64
        & $powerShellExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -Command $launcher
        $stepExitCode = $LASTEXITCODE
    }
    finally {
        $env:MPCVR_SETUP_STEP_SCRIPT = $previousScript
        $env:MPCVR_SETUP_STEP_PARAMS_B64 = $previousParameters
    }
    if ($stepExitCode -ne 0) {
        throw "$Name failed with exit code $stepExitCode."
    }
}

function New-RifeRuntimeBundle {
    param([Parameter(Mandatory = $true)][string]$Destination)

    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($archiveName in $rifeRuntimeArchiveNames) {
        Copy-Item -LiteralPath (Join-Path $payloadRoot $archiveName) -Destination (Join-Path $Destination $archiveName)
    }
    Copy-Item -LiteralPath $rifeRuntimeManifest -Destination (Join-Path $Destination 'runtime-manifest.json')

    @(
        foreach ($archiveName in $rifeRuntimeArchiveNames) {
            $hash = Get-ExpectedHash -ChecksumPath $payloadChecksums -FileName $archiveName
            "$hash  $archiveName"
        }
    ) | Set-Content -LiteralPath (Join-Path $Destination 'SHA256SUMS.txt') -Encoding ASCII
}

function Assert-MaxinePreflight {
    $runtimePath = Join-Path $env:LOCALAPPDATA 'MPCVR Maxine Runtime\nvvfx\libs'
    foreach ($fileName in @(
        'NVCVImage.dll',
        'NVVideoEffects.dll',
        'nvngxruntime.dll',
        'nvngx_vsr.dll',
        'nvVFXVideoSuperRes.dll'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $runtimePath $fileName) -PathType Leaf)) {
            throw "Maxine preflight is missing runtime file: $fileName"
        }
    }

    $userValue = [Environment]::GetEnvironmentVariable('NV_VIDEO_EFFECTS_PATH', 'User')
    if ([string]::IsNullOrWhiteSpace($userValue)) {
        throw 'Maxine preflight found no user NV_VIDEO_EFFECTS_PATH value.'
    }

    $expectedPath = [IO.Path]::GetFullPath($runtimePath).TrimEnd('\')
    $actualPath = [IO.Path]::GetFullPath($userValue).TrimEnd('\')
    if ($actualPath -ine $expectedPath) {
        throw "Maxine preflight expected NV_VIDEO_EFFECTS_PATH '$expectedPath' but found '$actualPath'."
    }

    return $expectedPath
}

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'MPCVR Maxine + RIFE Setup only supports Windows.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if (-not (Test-Path -LiteralPath $powerShellExecutable -PathType Leaf)) {
    Write-Host "Windows PowerShell 5.1 was not found at $powerShellExecutable." -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
    Write-Host 'LOCALAPPDATA is not available.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

$tempRoot = $null
$exitCode = 0
try {
    Assert-PayloadHashes

    Write-Host 'Verified all declared Maxine + RIFE setup payload hashes.' -ForegroundColor Green

    if ($ValidateOnly) {
        Invoke-SetupStep -Name 'Validating the Maxine runtime installer...' -ScriptPath $maxineRuntimeInstaller -Parameters @{
            ValidateOnly = $true
            NoPause = $true
        }

        $rifeValidationParameters = @{
            ValidateOnly = $true
            NoPause = $true
        }
        if (-not [string]::IsNullOrWhiteSpace($GpuInventoryJson)) {
            $rifeValidationParameters['GpuInventoryJson'] = $GpuInventoryJson
        }
        Invoke-SetupStep -Name 'Validating the RIFE runtime installer...' -ScriptPath $rifeRuntimeInstaller -Parameters $rifeValidationParameters

        Invoke-SetupStep `
            -Name 'Validating the renderer updater...' `
            -ScriptPath $rendererUpdater `
            -Parameters @{
                PackageArchive = $rendererArchive
                ChecksumFile = $payloadChecksums
                ValidateOnly = $true
                NoPause = $true
            }

        Write-Host
        Write-Host 'Unified Maxine + RIFE setup validation passed.' -ForegroundColor Green
        Complete-Run -ExitCode 0
    }

    if (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
        throw 'Close MPC-HC before running MPCVR Maxine + RIFE Setup.'
    }

    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("MPCVR-Maxine-RIFE-Setup-{0}" -f [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

    $stagedMaxineArchive = Join-Path $tempRoot 'MPCVR-Maxine-Runtime.zip'
    Copy-Item -LiteralPath $maxineRuntimeArchive -Destination $stagedMaxineArchive
    $maxineHash = Get-ExpectedHash -ChecksumPath $payloadChecksums -FileName 'MPCVR-Maxine-Runtime.zip'
    "$maxineHash  MPCVR-Maxine-Runtime.zip" | Set-Content -LiteralPath "$stagedMaxineArchive.sha256" -Encoding ASCII

    $rifeBundleRoot = Join-Path $tempRoot 'rife-runtime'
    New-RifeRuntimeBundle -Destination $rifeBundleRoot

    Invoke-SetupStep `
        -Name '1/4 Installing NVIDIA Maxine runtime...' `
        -ScriptPath $maxineRuntimeInstaller `
        -Parameters @{
            RuntimeArchive = $stagedMaxineArchive
            NoPause = $true
        }

    $rifeInstallParameters = @{
        RuntimeBundleRoot = $rifeBundleRoot
        ModelArchive = $rifeModelArchive
        NoPause = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($GpuInventoryJson)) {
        $rifeInstallParameters['GpuInventoryJson'] = $GpuInventoryJson
    }
    Invoke-SetupStep `
        -Name '2/4 Installing RIFE 4.6 TensorRT runtime...' `
        -ScriptPath $rifeRuntimeInstaller `
        -Parameters $rifeInstallParameters

    Invoke-SetupStep `
        -Name '3/4 Installing K-Lite restore shortcut...' `
        -ScriptPath $updaterInstaller `
        -Parameters @{ NoPause = $true }

    Invoke-SetupStep `
        -Name '4/4 Installing custom MPC Video Renderer into K-Lite...' `
        -ScriptPath $rendererUpdater `
        -Parameters @{
            PackageArchive = $rendererArchive
            ChecksumFile = $payloadChecksums
            NoPause = $true
        }

    $rifeInstallRoot = Join-Path $env:LOCALAPPDATA 'MPCVideoRenderer\RIFE'
    $rifePreflightParameters = @{ InstallRoot = $rifeInstallRoot }
    if (-not [string]::IsNullOrWhiteSpace($GpuInventoryJson)) {
        $rifePreflightParameters['GpuInventoryJson'] = $GpuInventoryJson
    }
    Invoke-SetupStep -Name 'Running RIFE post-install preflight...' -ScriptPath $rifePreflight -Parameters $rifePreflightParameters
    $maxineRuntimePath = Assert-MaxinePreflight

    Write-Host
    Write-Host 'MPCVR Maxine + RIFE Setup completed successfully.' -ForegroundColor Green
    Write-Host
    Write-Host 'Open MPC-HC and play a video.'
    Write-Host 'Press Ctrl+J and confirm:'
    Write-Host "  Maxine runtime: $maxineRuntimePath"
    Write-Host '  RIFE runtime: ready'
    Write-Host '  RIFE model: 4.6'
    Write-Host 'First playback may build a TensorRT engine under %LOCALAPPDATA%\MPCVideoRenderer\RIFE\cache.'
}
catch {
    $exitCode = 1
    Write-Host
    Write-Host "Setup failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}
finally {
    if ($tempRoot -and (Test-Path -LiteralPath $tempRoot)) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Complete-Run -ExitCode $exitCode
