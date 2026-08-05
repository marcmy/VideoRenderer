#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$OutputPath,
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$modulePath = Join-Path $PSScriptRoot 'MpcvrSetup.Common.psm1'
Import-Module -Name $modulePath -Force

try {
    $profile = Get-MpcvrSystemProfile

    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $profileRoot = Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup'
        $OutputPath = Join-Path $profileRoot 'system-profile.json'
    }

    $savedPath = Save-MpcvrSystemProfile -Profile $profile -Path $OutputPath

    Write-Host 'MPCVR system profile captured.' -ForegroundColor Green
    Write-Host "Saved to: $savedPath"
    Write-Host

    $profile.Gpus | Format-Table Name, DriverVersion, MemoryMiB, CurrentWidth, CurrentHeight, CurrentRefreshHz -AutoSize
    $profile.Players | Format-Table Name, DirectoryExists, RendererExists, RendererVersion -AutoSize
    $profile.Runtimes | Format-List

    if ($PassThru) {
        return $profile
    }
}
catch {
    Write-Host "System-profile capture failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
    exit 1
}
