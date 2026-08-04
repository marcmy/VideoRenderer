#requires -Version 5.1

[CmdletBinding()]
param()

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

$sourceUpdater = Join-Path $PSScriptRoot 'Update-KLiteMPCVR.ps1'
if (-not (Test-Path -LiteralPath $sourceUpdater -PathType Leaf)) {
    throw "Update-KLiteMPCVR.ps1 was not found beside this installer."
}

$installDirectory = Join-Path $env:LOCALAPPDATA 'MPCVR Maxine Updater'
$installedUpdater = Join-Path $installDirectory 'Update-KLiteMPCVR.ps1'
$desktop = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktop 'Restore MPC-VR Maxine.lnk'
$powerShellExecutable = Get-PowerShellExecutable

New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
Copy-Item -LiteralPath $sourceUpdater -Destination $installedUpdater -Force

$wshShell = New-Object -ComObject WScript.Shell
$shortcut = $wshShell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $powerShellExecutable
$shortcut.Arguments = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $installedUpdater
$shortcut.WorkingDirectory = $installDirectory
$shortcut.Description = 'Download and restore the latest custom MPC Video Renderer Maxine build in K-Lite Codec Pack'

$mpcIcon = 'C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\mpc-hc64.exe'
if (Test-Path -LiteralPath $mpcIcon -PathType Leaf) {
    $shortcut.IconLocation = "$mpcIcon,0"
}

$shortcut.Save()

Write-Host 'Installed the updater and created this desktop shortcut:' -ForegroundColor Green
Write-Host $shortcutPath
Write-Host
Write-Host "PowerShell host: $powerShellExecutable"
Write-Host 'Double-click the shortcut after K-Lite updates overwrite your custom MPC-VR files.'
Write-Host 'The updater will request administrator permission, download the newest rolling build, verify its SHA-256, and replace both K-Lite copies.'
Write-Host
[void](Read-Host 'Press Enter to close')
