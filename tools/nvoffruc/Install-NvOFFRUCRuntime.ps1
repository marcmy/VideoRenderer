#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$SdkPath,
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR NvOFFRUC Runtime'),
    [switch]$ValidateOnly,
    [switch]$SkipEnvironmentUpdate,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 2.0

function Complete-Run {
    param([int]$ExitCode)

    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

function Select-SdkPackage {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select Optical_Flow_SDK_5.0.7.zip'
    $dialog.Filter = 'NVIDIA Optical Flow SDK ZIP (*.zip)|*.zip|All files (*.*)|*.*'
    $dialog.CheckFileExists = $true
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        throw 'No SDK package was selected.'
    }
    return $dialog.FileName
}

function Get-NvOffrucSource {
    param(
        [Parameter(Mandatory)]
        [string]$SearchRoot
    )

    $runtime = Get-ChildItem -LiteralPath $SearchRoot -Recurse -File -Filter 'NvOFFRUC.dll' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '[\\/]NvOFFRUC[\\/]NvOFFRUCSample[\\/]bin[\\/]win64[\\/]' } |
        Select-Object -First 1
    if (-not $runtime) {
        throw 'The x64 NvOFFRUC.dll was not found in the selected SDK package.'
    }

    $cuda = Join-Path $runtime.DirectoryName 'cudart64_110.dll'
    if (-not (Test-Path -LiteralPath $cuda -PathType Leaf)) {
        throw 'cudart64_110.dll was not found beside NvOFFRUC.dll.'
    }

    return [pscustomobject]@{
        NvOffruc = $runtime.FullName
        CudaRuntime = $cuda
    }
}

function Publish-EnvironmentChange {
    if ($SkipEnvironmentUpdate) {
        return
    }

    try {
        if (-not ('MpcvrNvOffruc.EnvironmentBroadcast' -as [type])) {
            Add-Type @'
using System;
using System.Runtime.InteropServices;
namespace MpcvrNvOffruc {
    public static class EnvironmentBroadcast {
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr SendMessageTimeout(
            IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
            uint flags, uint timeout, out UIntPtr result);
    }
}
'@
        }
        $result = [UIntPtr]::Zero
        [void][MpcvrNvOffruc.EnvironmentBroadcast]::SendMessageTimeout(
            [IntPtr]0xffff, 0x001A, [UIntPtr]::Zero, 'Environment',
            2, 5000, [ref]$result)
    }
    catch {
        Write-Warning "NV_OFFRUC_PATH was updated, but the live environment refresh failed: $($_.Exception.Message)"
    }
}

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'The NvOFFRUC runtime installer only supports Windows.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

$exitCode = 0
$tempRoot = $null
$staging = $null
$backup = $null
$existingMoved = $false
$newInstalled = $false
$previousUserValue = [Environment]::GetEnvironmentVariable('NV_OFFRUC_PATH', 'User')

try {
    $InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
    if (-not [IO.Path]::IsPathRooted($InstallRoot)) {
        throw "InstallRoot must be fully qualified: $InstallRoot"
    }

    if ($ValidateOnly) {
        if (-not [string]::IsNullOrWhiteSpace($SdkPath)) {
            $resolvedValidationPath = (Resolve-Path -LiteralPath $SdkPath).Path
            if (Test-Path -LiteralPath $resolvedValidationPath -PathType Leaf) {
                if ([IO.Path]::GetExtension($resolvedValidationPath) -ine '.zip') {
                    throw 'SdkPath must be the NVIDIA Optical Flow SDK ZIP or its extracted directory.'
                }
                $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('MPCVR-NvOFFRUC-Validate-' + [guid]::NewGuid().ToString('N'))
                New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
                Expand-Archive -LiteralPath $resolvedValidationPath -DestinationPath $tempRoot -Force
                [void](Get-NvOffrucSource -SearchRoot $tempRoot)
            }
            else {
                [void](Get-NvOffrucSource -SearchRoot $resolvedValidationPath)
            }
        }

        Write-Host 'NvOFFRUC runtime installer validation passed.' -ForegroundColor Green
        Complete-Run -ExitCode 0
    }

    if (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
        throw 'Close MPC-HC before installing the NvOFFRUC runtime.'
    }

    if ([string]::IsNullOrWhiteSpace($SdkPath)) {
        $SdkPath = Select-SdkPackage
    }

    $resolved = (Resolve-Path -LiteralPath $SdkPath).Path
    if (Test-Path -LiteralPath $resolved -PathType Leaf) {
        if ([IO.Path]::GetExtension($resolved) -ine '.zip') {
            throw 'SdkPath must be the NVIDIA Optical Flow SDK ZIP or its extracted directory.'
        }
        $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('MPCVR-NvOFFRUC-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
        Expand-Archive -LiteralPath $resolved -DestinationPath $tempRoot -Force
        $searchRoot = $tempRoot
    }
    else {
        $searchRoot = $resolved
    }

    $source = Get-NvOffrucSource -SearchRoot $searchRoot
    $staging = "$InstallRoot.staging-$([guid]::NewGuid().ToString('N'))"
    $backup = "$InstallRoot.backup"
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $staging -Force | Out-Null
    Copy-Item -LiteralPath $source.NvOffruc -Destination $staging -Force
    Copy-Item -LiteralPath $source.CudaRuntime -Destination $staging -Force

    foreach ($fileName in @('NvOFFRUC.dll', 'cudart64_110.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $staging $fileName) -PathType Leaf)) {
            throw "The staged NvOFFRUC runtime is missing $fileName."
        }
    }

    Remove-Item -LiteralPath $backup -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $InstallRoot -PathType Container) {
        Move-Item -LiteralPath $InstallRoot -Destination $backup
        $existingMoved = $true
    }

    Move-Item -LiteralPath $staging -Destination $InstallRoot
    $newInstalled = $true

    if (-not $SkipEnvironmentUpdate) {
        [Environment]::SetEnvironmentVariable('NV_OFFRUC_PATH', $InstallRoot, 'User')
        $env:NV_OFFRUC_PATH = $InstallRoot
        Publish-EnvironmentChange
    }

    foreach ($fileName in @('NvOFFRUC.dll', 'cudart64_110.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $InstallRoot $fileName) -PathType Leaf)) {
            throw "Post-install verification failed for $fileName."
        }
    }

    if ($existingMoved) {
        Remove-Item -LiteralPath $backup -Recurse -Force -ErrorAction SilentlyContinue
        $existingMoved = $false
    }

    Write-Host
    Write-Host 'NvOFFRUC runtime installed successfully.' -ForegroundColor Green
    Write-Host "Location: $InstallRoot"
    Write-Host 'Restart MPC-HC before testing frame interpolation.'
    Write-Host
    Get-FileHash -Algorithm SHA256 -LiteralPath `
        (Join-Path $InstallRoot 'NvOFFRUC.dll'), `
        (Join-Path $InstallRoot 'cudart64_110.dll') |
        Format-Table Path, Hash -AutoSize
}
catch {
    $exitCode = 1

    if ($newInstalled -and (Test-Path -LiteralPath $InstallRoot)) {
        Remove-Item -LiteralPath $InstallRoot -Recurse -Force -ErrorAction SilentlyContinue
        $newInstalled = $false
    }
    if ($existingMoved -and (Test-Path -LiteralPath $backup)) {
        Move-Item -LiteralPath $backup -Destination $InstallRoot -ErrorAction SilentlyContinue
        $existingMoved = $false
    }
    if (-not $SkipEnvironmentUpdate) {
        [Environment]::SetEnvironmentVariable('NV_OFFRUC_PATH', $previousUserValue, 'User')
        $env:NV_OFFRUC_PATH = $previousUserValue
        Publish-EnvironmentChange
    }

    Write-Host
    Write-Host "NvOFFRUC runtime installation failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}
finally {
    if (-not [string]::IsNullOrWhiteSpace($tempRoot)) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (-not [string]::IsNullOrWhiteSpace($staging)) {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Complete-Run -ExitCode $exitCode
