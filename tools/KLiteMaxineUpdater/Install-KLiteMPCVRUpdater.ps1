#requires -Version 7.0

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$sourceUpdater = Join-Path $PSScriptRoot 'Update-KLiteMPCVR.ps1'
if (-not (Test-Path -LiteralPath $sourceUpdater -PathType Leaf)) {
    throw "Update-KLiteMPCVR.ps1 was not found beside this installer."
}

$installDirectory = Join-Path $env:LOCALAPPDATA 'MPCVR Maxine Updater'
$installedUpdater = Join-Path $installDirectory 'Update-KLiteMPCVR.ps1'
$desktop = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktop 'Restore MPC-VR Maxine.lnk'
$pwsh = (Get-Command pwsh.exe -ErrorAction Stop).Source

New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
Copy-Item -LiteralPath $sourceUpdater -Destination $installedUpdater -Force

$wshShell = New-Object -ComObject WScript.Shell
$shortcut = $wshShell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $pwsh
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
Write-Host 'Double-click it after K-Lite updates overwrite your custom MPC-VR files.'
Write-Host 'The updater will request administrator permission, download the newest rolling build, verify its SHA-256, and replace both K-Lite copies.'
Write-Host
[void](Read-Host 'Press Enter to close')
