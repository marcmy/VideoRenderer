#requires -Version 5.1

[CmdletBinding()]
param(
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'

function Get-PowerShellExecutable {
    $pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
    if ($null -ne $pwsh) {
        return $pwsh.Source
    }

    $windowsPowerShell = Get-Command powershell.exe -ErrorAction SilentlyContinue
    if ($null -ne $windowsPowerShell) {
        return $windowsPowerShell.Source
    }

    throw 'Neither PowerShell 7 (pwsh.exe) nor Windows PowerShell (powershell.exe) was found.'
}

$payload = @('Update-MPCVR.ps1', 'Update-KLiteMPCVR.ps1', 'MPCVRPlayerInstall.psm1')
foreach ($name in $payload) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $name) -PathType Leaf)) {
        throw "$name was not found beside this installer."
    }
}

$installDirectory = Join-Path $env:LOCALAPPDATA 'MPCVR Custom Updater'
$installedUpdater = Join-Path $installDirectory 'Update-MPCVR.ps1'
$installedModule = Join-Path $installDirectory 'MPCVRPlayerInstall.psm1'
$desktop = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktop 'Restore MPC-VR Maxine + RIFE.lnk'
$legacyShortcutPath = Join-Path $desktop 'Restore MPC-VR Maxine.lnk'
$powerShellExecutable = Get-PowerShellExecutable

New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
foreach ($name in $payload) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $installDirectory $name) -Force
}
Remove-Item -LiteralPath (Join-Path $installDirectory 'KLiteRendererInstall.psm1') -Force -ErrorAction SilentlyContinue
Import-Module $installedModule -Force

$wshShell = New-Object -ComObject WScript.Shell
$shortcut = $wshShell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $powerShellExecutable
$shortcut.Arguments = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $installedUpdater
$shortcut.WorkingDirectory = $installDirectory
$shortcut.Description = 'Download and restore the latest custom MPC Video Renderer Maxine + RIFE build in MPC-HC, MPC-BE and K-Lite Codec Pack'

$mpcIcon = @(Get-MPCVRPlayerTarget | Where-Object { $_.Executable -and (Test-Path -LiteralPath $_.Executable -PathType Leaf) } | Select-Object -First 1)
if ($mpcIcon.Count) {
    $shortcut.IconLocation = "$($mpcIcon[0].Executable),0"
}

$shortcut.Save()
if (-not (Test-Path -LiteralPath $shortcutPath -PathType Leaf)) {
    throw "The new desktop shortcut was not created: $shortcutPath"
}
if (Test-Path -LiteralPath $legacyShortcutPath -PathType Leaf) {
    Remove-Item -LiteralPath $legacyShortcutPath -Force
}

Write-Host 'Installed the updater and created this desktop shortcut:' -ForegroundColor Green
Write-Host $shortcutPath
Write-Host
Write-Host "PowerShell host: $powerShellExecutable"
Write-Host 'Double-click the shortcut after a player or K-Lite update overwrites your custom MPC-VR files, or after installing another player.'
Write-Host 'The updater will request administrator permission, download the newest Maxine + RIFE build, verify its SHA-256, and transactionally install it for each detected player.'

if (-not $NoPause) {
    Write-Host
    [void](Read-Host 'Press Enter to close')
}
