#requires -version 5.1
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$SdkPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

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

if ([string]::IsNullOrWhiteSpace($SdkPath)) {
    $SdkPath = Select-SdkPackage
}

$resolved = (Resolve-Path -LiteralPath $SdkPath).Path
$tempRoot = $null
try {
    if (Test-Path -LiteralPath $resolved -PathType Leaf) {
        if ([IO.Path]::GetExtension($resolved) -ne '.zip') {
            throw 'SdkPath must be the NVIDIA Optical Flow SDK ZIP or its extracted directory.'
        }
        $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('MPCVR-NvOFFRUC-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $tempRoot | Out-Null
        Expand-Archive -LiteralPath $resolved -DestinationPath $tempRoot -Force
        $searchRoot = $tempRoot
    }
    else {
        $searchRoot = $resolved
    }

    $runtime = Get-ChildItem -LiteralPath $searchRoot -Recurse -File -Filter 'NvOFFRUC.dll' |
        Where-Object { $_.FullName -match '[\\/]NvOFFRUC[\\/]NvOFFRUCSample[\\/]bin[\\/]win64[\\/]' } |
        Select-Object -First 1
    if (-not $runtime) {
        throw 'The x64 NvOFFRUC.dll was not found in the selected SDK package.'
    }

    $cuda = Join-Path $runtime.DirectoryName 'cudart64_110.dll'
    if (-not (Test-Path -LiteralPath $cuda -PathType Leaf)) {
        throw 'cudart64_110.dll was not found beside NvOFFRUC.dll.'
    }

    $destination = Join-Path $env:LOCALAPPDATA 'MPCVR NvOFFRUC Runtime'
    $staging = $destination + '.new'
    $backup = $destination + '.old'
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $staging | Out-Null
    Copy-Item -LiteralPath $runtime.FullName -Destination $staging
    Copy-Item -LiteralPath $cuda -Destination $staging

    if (Test-Path -LiteralPath $backup) {
        Remove-Item -LiteralPath $backup -Recurse -Force
    }
    if (Test-Path -LiteralPath $destination) {
        Move-Item -LiteralPath $destination -Destination $backup
    }
    try {
        Move-Item -LiteralPath $staging -Destination $destination
    }
    catch {
        if (Test-Path -LiteralPath $backup) {
            Move-Item -LiteralPath $backup -Destination $destination
        }
        throw
    }
    Remove-Item -LiteralPath $backup -Recurse -Force -ErrorAction SilentlyContinue

    [Environment]::SetEnvironmentVariable('NV_OFFRUC_PATH', $destination, 'User')
    $env:NV_OFFRUC_PATH = $destination

    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class EnvironmentBroadcast {
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr SendMessageTimeout(
        IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
        uint flags, uint timeout, out UIntPtr result);
}
'@
    $result = [UIntPtr]::Zero
    [void][EnvironmentBroadcast]::SendMessageTimeout(
        [IntPtr]0xffff, 0x001A, [UIntPtr]::Zero, 'Environment',
        2, 5000, [ref]$result)

    Write-Host ''
    Write-Host 'NvOFFRUC runtime installed successfully.' -ForegroundColor Green
    Write-Host "Location: $destination"
    Write-Host 'Restart MPC-HC before testing frame interpolation.'
    Write-Host ''
    Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $destination 'NvOFFRUC.dll'), (Join-Path $destination 'cudart64_110.dll') |
        Format-Table Path, Hash -AutoSize
}
finally {
    if ($tempRoot -and (Test-Path -LiteralPath $tempRoot)) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
