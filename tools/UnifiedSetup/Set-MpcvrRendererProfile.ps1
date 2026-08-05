#requires -Version 5.1

[CmdletBinding()]
param(
    [ValidateSet('Preview', 'Apply', 'Restore', 'Show')]
    [string]$Action = 'Preview',
    [string]$ProfileName,
    [string]$ProfilePath,
    [string]$ProfileRoot,
    [string]$RegistryPath = 'HKCU:\Software\MPCVideoRenderer',
    [string]$BackupPath,
    [switch]$AllowPlayerRunning,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$profilesModule = Join-Path $PSScriptRoot 'MpcvrSetup.Profiles.psm1'
$settingsModule = Join-Path $PSScriptRoot 'MpcvrSetup.RendererSettings.psm1'
foreach ($path in @($profilesModule, $settingsModule)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Renderer profile dependency is missing: $path"
    }
}
Import-Module -Name $profilesModule -Force
Import-Module -Name $settingsModule -Force

function Get-SelectedProfile {
    if (-not [string]::IsNullOrWhiteSpace($ProfilePath)) {
        return [pscustomobject]@{
            Path = (Resolve-Path -LiteralPath $ProfilePath).Path
            Profile = Read-MpcvrProfile -Path $ProfilePath
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($ProfileName)) {
        return Find-MpcvrProfile -Name $ProfileName -ProfileRoot $ProfileRoot
    }
    throw 'ProfileName or ProfilePath is required.'
}

if ($Action -eq 'Restore') {
    if ([string]::IsNullOrWhiteSpace($BackupPath)) {
        throw 'BackupPath is required for Restore.'
    }
    $restored = Restore-MpcvrRendererSettingsBackup `
        -BackupPath $BackupPath `
        -RegistryPath $RegistryPath `
        -AllowPlayerRunning:$AllowPlayerRunning
    $result = [pscustomobject]@{
        Action = 'Restored'
        RegistryPath = $RegistryPath
        BackupPath = (Resolve-Path -LiteralPath $BackupPath).Path
        Settings = $restored.Settings
    }
}
elseif ($Action -eq 'Show') {
    $current = Get-MpcvrRendererSettings -RegistryPath $RegistryPath
    $result = [pscustomobject]@{
        Action = 'CurrentSettings'
        RegistryPath = $RegistryPath
        Settings = $current.Settings
        Values = $current.Values
    }
}
else {
    $selected = Get-SelectedProfile
    $desired = ConvertTo-MpcvrRendererSettings -Profile $selected.Profile
    $applyResult = Set-MpcvrRendererSettings `
        -Settings $desired `
        -RegistryPath $RegistryPath `
        -BackupPath $BackupPath `
        -DryRun:($Action -eq 'Preview') `
        -AllowPlayerRunning:$AllowPlayerRunning

    $result = [pscustomobject]@{
        Action = if ($Action -eq 'Apply') { 'Applied' } else { 'Preview' }
        ProfileName = [string]$selected.Profile.name
        ProfilePath = [string]$selected.Path
        ProfileLocked = [bool]$selected.Profile.locked
        RegistryPath = $RegistryPath
        BackupPath = $applyResult.BackupPath
        Applied = [bool]$applyResult.Applied
        Changes = $applyResult.Changes
        Settings = [pscustomobject]$desired
    }

    if ($Action -eq 'Apply') {
        $stateRoot = Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup'
        New-Item -ItemType Directory -Path $stateRoot -Force | Out-Null
        $activeState = [pscustomobject]@{
            SchemaVersion = 1
            AppliedAtUtc = [DateTime]::UtcNow.ToString('o')
            ProfileName = [string]$selected.Profile.name
            ProfilePath = [string]$selected.Path
            ProfileLocked = [bool]$selected.Profile.locked
            RegistryPath = $RegistryPath
            BackupPath = $applyResult.BackupPath
            Settings = [pscustomobject]$desired
        }
        $activeState | ConvertTo-Json -Depth 8 |
            Set-Content -LiteralPath (Join-Path $stateRoot 'active-renderer-profile.json') -Encoding UTF8
    }
}

if ($Json) {
    $result | ConvertTo-Json -Depth 10
}
else {
    if ($result.Action -eq 'Preview') {
        Write-Host "Profile preview: $($result.ProfileName)" -ForegroundColor Cyan
        if (@($result.Changes).Count -eq 0) {
            Write-Host 'No renderer setting changes are required.' -ForegroundColor Green
        }
        else {
            $result.Changes | Format-Table Name, Before, After -AutoSize
        }
    }
    elseif ($result.Action -eq 'Applied') {
        Write-Host "Applied renderer profile: $($result.ProfileName)" -ForegroundColor Green
        Write-Host "Backup: $($result.BackupPath)"
        Write-Host 'Restart MPC-HC before testing the new settings.'
    }
    elseif ($result.Action -eq 'Restored') {
        Write-Host "Renderer settings restored from: $($result.BackupPath)" -ForegroundColor Green
    }
    else {
        $result.Settings | Format-List
    }
}
