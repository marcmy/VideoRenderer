#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RuntimeArchive,
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR Maxine Runtime'),
    [switch]$NoPause,
    [switch]$ValidateOnly,
    [switch]$SkipEnvironmentUpdate
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$releaseBaseUrl = 'https://github.com/marcmy/VideoRenderer/releases/latest/download'
$assetName = 'MPCVR-Maxine-Runtime.zip'
$checksumName = "$assetName.sha256"
$requiredFiles = @(
    'NVCVImage.dll',
    'NVVideoEffects.dll',
    'nvngxruntime.dll',
    'nvngx_vsr.dll',
    'nvVFXVideoSuperRes.dll'
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
        [string]$ChecksumPath
    )

    $text = Get-Content -LiteralPath $ChecksumPath -Raw
    $match = [regex]::Match($text, '(?i)\b[a-f0-9]{64}\b')
    if (-not $match.Success) {
        throw "The checksum file is malformed: $ChecksumPath"
    }
    return $match.Value.ToLowerInvariant()
}

function Test-ArchiveHash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArchivePath,
        [Parameter(Mandatory = $true)]
        [string]$ChecksumPath
    )

    $expectedHash = Get-ExpectedHash -ChecksumPath $ChecksumPath
    $actualHash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "SHA-256 verification failed. Expected $expectedHash but found $actualHash."
    }
    return $actualHash
}

function Get-CompatibleRuntimeDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SearchRoot
    )

    $imageLibraries = Get-ChildItem -LiteralPath $SearchRoot -Filter 'NVCVImage.dll' -File -Recurse -ErrorAction SilentlyContinue
    foreach ($imageLibrary in $imageLibraries) {
        $candidate = $imageLibrary.Directory.FullName
        $missing = @($requiredFiles | Where-Object {
            -not (Test-Path -LiteralPath (Join-Path $candidate $_) -PathType Leaf)
        })
        if ($missing.Count -eq 0) {
            return $candidate
        }
    }

    throw ('No compatible VideoSuperRes runtime directory was found. Required files: {0}' -f ($requiredFiles -join ', '))
}

function Publish-EnvironmentChange {
    if ($SkipEnvironmentUpdate) {
        return
    }

    try {
        if (-not ('MpcVrMaxine.NativeMethods' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace MpcVrMaxine
{
    public static class NativeMethods
    {
        public const int HWND_BROADCAST = 0xffff;
        public const int WM_SETTINGCHANGE = 0x001A;
        public const int SMTO_ABORTIFHUNG = 0x0002;

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr SendMessageTimeout(
            IntPtr hWnd,
            int Msg,
            IntPtr wParam,
            string lParam,
            int fuFlags,
            int uTimeout,
            out IntPtr lpdwResult);
    }
}
'@
        }

        $result = [IntPtr]::Zero
        [void][MpcVrMaxine.NativeMethods]::SendMessageTimeout(
            [IntPtr][MpcVrMaxine.NativeMethods]::HWND_BROADCAST,
            [MpcVrMaxine.NativeMethods]::WM_SETTINGCHANGE,
            [IntPtr]::Zero,
            'Environment',
            [MpcVrMaxine.NativeMethods]::SMTO_ABORTIFHUNG,
            5000,
            [ref]$result)
    }
    catch {
        Write-Warning "The user environment variable was updated, but Windows did not accept the live refresh notification: $($_.Exception.Message)"
        Write-Warning 'Restarting MPC-HC after a Windows sign-out, restart, or Explorer restart will still apply it.'
    }
}

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'This installer only supports Windows.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
    Write-Host 'LOCALAPPDATA is not available.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
if (-not [IO.Path]::IsPathRooted($InstallRoot)) {
    Write-Host "The install directory must be fully qualified: $InstallRoot" -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if ($ValidateOnly) {
    if ($requiredFiles.Count -ne (@($requiredFiles | Select-Object -Unique)).Count) {
        throw 'The required runtime file list contains duplicates.'
    }
    if (-not $SkipEnvironmentUpdate) {
        Publish-EnvironmentChange
        if (-not ('MpcVrMaxine.NativeMethods' -as [type])) {
            throw 'The environment notification helper could not be loaded.'
        }
    }
    Write-Host 'Maxine runtime installer validation passed.' -ForegroundColor Green
    Complete-Run -ExitCode 0
}

$runningPlayers = Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue
if ($runningPlayers) {
    Write-Host 'Close MPC-HC before installing the Maxine runtime.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if ([Net.ServicePointManager]::SecurityProtocol -band [Net.SecurityProtocolType]::Tls12) {
    # TLS 1.2 is already enabled.
}
else {
    [Net.ServicePointManager]::SecurityProtocol =
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("MPCVR-Maxine-Runtime-{0}" -f [guid]::NewGuid())
$downloadedArchive = Join-Path $tempRoot $assetName
$downloadedChecksum = Join-Path $tempRoot $checksumName
$extractRoot = Join-Path $tempRoot 'extracted'
$stagingRoot = "$InstallRoot.staging-$([guid]::NewGuid().ToString('N'))"
$backupRoot = "$InstallRoot.backup"
$runtimeDirectory = Join-Path $InstallRoot 'nvvfx\libs'
$previousUserValue = [Environment]::GetEnvironmentVariable('NV_VIDEO_EFFECTS_PATH', 'User')
$existingMoved = $false
$newInstalled = $false
$exitCode = 0

try {
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

    if ([string]::IsNullOrWhiteSpace($RuntimeArchive)) {
        Write-Host 'Downloading the MPC-VR Maxine runtime bundle...'
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$releaseBaseUrl/$assetName" -OutFile $downloadedArchive
            Invoke-WebRequest -UseBasicParsing -Uri "$releaseBaseUrl/$checksumName" -OutFile $downloadedChecksum
        }
        catch {
            throw 'The Maxine runtime bundle is not available in the latest VideoRenderer release yet.'
        }
    }
    else {
        $resolvedArchive = (Resolve-Path -LiteralPath $RuntimeArchive).Path
        Copy-Item -LiteralPath $resolvedArchive -Destination $downloadedArchive -Force

        $adjacentChecksum = "$resolvedArchive.sha256"
        if (-not (Test-Path -LiteralPath $adjacentChecksum -PathType Leaf)) {
            $adjacentChecksum = Join-Path ([IO.Path]::GetDirectoryName($resolvedArchive)) $checksumName
        }
        if (-not (Test-Path -LiteralPath $adjacentChecksum -PathType Leaf)) {
            throw "A SHA-256 file was not found beside the local runtime archive. Expected $checksumName."
        }
        Copy-Item -LiteralPath $adjacentChecksum -Destination $downloadedChecksum -Force
    }

    $verifiedHash = Test-ArchiveHash -ArchivePath $downloadedArchive -ChecksumPath $downloadedChecksum
    Write-Host "Verified runtime archive: $verifiedHash"

    New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
    Expand-Archive -LiteralPath $downloadedArchive -DestinationPath $extractRoot -Force
    $sourceRuntime = Get-CompatibleRuntimeDirectory -SearchRoot $extractRoot

    $stagedRuntime = Join-Path $stagingRoot 'nvvfx\libs'
    New-Item -ItemType Directory -Path $stagedRuntime -Force | Out-Null
    Get-ChildItem -LiteralPath $sourceRuntime -Force | Copy-Item -Destination $stagedRuntime -Recurse -Force

    foreach ($fileName in $requiredFiles) {
        $filePath = Join-Path $stagedRuntime $fileName
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            throw "The staged runtime is missing $fileName."
        }
    }

    if (Test-Path -LiteralPath $backupRoot) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $InstallRoot) {
        Move-Item -LiteralPath $InstallRoot -Destination $backupRoot
        $existingMoved = $true
    }

    Move-Item -LiteralPath $stagingRoot -Destination $InstallRoot
    $newInstalled = $true

    if (-not $SkipEnvironmentUpdate) {
        [Environment]::SetEnvironmentVariable('NV_VIDEO_EFFECTS_PATH', $runtimeDirectory, 'User')
        $env:NV_VIDEO_EFFECTS_PATH = $runtimeDirectory
        Publish-EnvironmentChange
    }

    foreach ($fileName in $requiredFiles) {
        $installedFile = Join-Path $runtimeDirectory $fileName
        if (-not (Test-Path -LiteralPath $installedFile -PathType Leaf)) {
            throw "Post-install verification failed for $fileName."
        }
    }

    if ($existingMoved -and (Test-Path -LiteralPath $backupRoot)) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force
        $existingMoved = $false
    }

    Write-Host
    Write-Host 'NVIDIA Maxine VideoSuperRes runtime installed successfully.' -ForegroundColor Green
    Write-Host "Runtime directory: $runtimeDirectory"
    if (-not $SkipEnvironmentUpdate) {
        Write-Host "User variable NV_VIDEO_EFFECTS_PATH: $runtimeDirectory"
    }
    Write-Host
    Write-Host 'Restart MPC-HC before testing Maxine. Ctrl+J should report the loaded Maxine runtime path.'

    $requiredFiles | ForEach-Object {
        $item = Get-Item -LiteralPath (Join-Path $runtimeDirectory $_)
        [pscustomobject]@{
            File = $item.Name
            Version = $item.VersionInfo.FileVersion
            SizeMB = [math]::Round($item.Length / 1MB, 2)
        }
    } | Format-Table -AutoSize
}
catch {
    $exitCode = 1

    if ($newInstalled -and (Test-Path -LiteralPath $InstallRoot)) {
        Remove-Item -LiteralPath $InstallRoot -Recurse -Force -ErrorAction SilentlyContinue
        $newInstalled = $false
    }
    if ($existingMoved -and (Test-Path -LiteralPath $backupRoot)) {
        Move-Item -LiteralPath $backupRoot -Destination $InstallRoot -ErrorAction SilentlyContinue
        $existingMoved = $false
    }
    if (-not $SkipEnvironmentUpdate) {
        [Environment]::SetEnvironmentVariable('NV_VIDEO_EFFECTS_PATH', $previousUserValue, 'User')
        $env:NV_VIDEO_EFFECTS_PATH = $previousUserValue
        Publish-EnvironmentChange
    }

    Write-Host
    Write-Host "Runtime installation failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Complete-Run -ExitCode $exitCode
