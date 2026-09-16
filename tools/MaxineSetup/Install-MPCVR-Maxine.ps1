#requires -Version 5.1

[CmdletBinding()]
param(
    [switch]$NoPause,
    [switch]$ValidateOnly,
    [string]$GpuInventoryJson
)

$ErrorActionPreference = 'Stop'
$unifiedInstaller = Join-Path $PSScriptRoot 'Install-MPCVR-Maxine-RIFE.ps1'
$unifiedPayload = Join-Path $PSScriptRoot 'payload\MPCVR-RIFE-Model-v4.6.zip'

Write-Host 'This setup package is now MPCVR Maxine + RIFE.' -ForegroundColor Cyan

if (-not (Test-Path -LiteralPath $unifiedInstaller -PathType Leaf) -or
    -not (Test-Path -LiteralPath $unifiedPayload -PathType Leaf)) {
    Write-Host 'The unified Maxine + RIFE setup payload is not present beside this compatibility entry point.' -ForegroundColor Red
    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit 1
}

$parameters = @{
    NoPause = [bool]$NoPause
    ValidateOnly = [bool]$ValidateOnly
}
if (-not [string]::IsNullOrWhiteSpace($GpuInventoryJson)) {
    $parameters['GpuInventoryJson'] = $GpuInventoryJson
}

& $unifiedInstaller @parameters
exit $LASTEXITCODE
