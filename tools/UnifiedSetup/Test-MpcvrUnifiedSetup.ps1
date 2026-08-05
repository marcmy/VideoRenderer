#requires -Version 5.1

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$modulePath = Join-Path $PSScriptRoot 'MpcvrSetup.Common.psm1'
$schemaPath = Join-Path $PSScriptRoot 'profiles\profile.schema.json'

if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    throw "Missing module: $modulePath"
}
if (-not (Test-Path -LiteralPath $schemaPath -PathType Leaf)) {
    throw "Missing profile schema: $schemaPath"
}

Import-Module -Name $modulePath -Force

$requiredCommands = @(
    'Get-MpcvrGpuProfile',
    'Get-MpcvrDisplayProfile',
    'Get-MpcvrRuntimeStatus',
    'Get-MpcvrPlayerTargets',
    'Get-MpcvrSystemProfile',
    'Save-MpcvrSystemProfile'
)

foreach ($commandName in $requiredCommands) {
    if (-not (Get-Command -Name $commandName -Module MpcvrSetup.Common -ErrorAction SilentlyContinue)) {
        throw "The unified setup module does not export $commandName."
    }
}

$schema = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json
if ($schema.title -ne 'MPCVR Unified Setup Profile') {
    throw 'The profile schema title is missing or incorrect.'
}
if ($schema.properties.mode.enum.Count -ne 3) {
    throw 'The profile schema must expose Automatic, Guided, and Advanced modes.'
}
if ($schema.properties.locked.type -ne 'boolean') {
    throw 'The profile schema must preserve manually locked profiles.'
}

$profile = Get-MpcvrSystemProfile
if ($profile.SchemaVersion -ne 1) {
    throw 'Unexpected system-profile schema version.'
}
if ($null -eq $profile.Gpus -or $null -eq $profile.Runtimes -or $null -eq $profile.Players) {
    throw 'The system profile is incomplete.'
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('MPCVR-UnifiedSetup-Test-' + [guid]::NewGuid().ToString('N'))
try {
    $outputPath = Join-Path $tempRoot 'system-profile.json'
    $savedPath = Save-MpcvrSystemProfile -Profile $profile -Path $outputPath
    if (-not (Test-Path -LiteralPath $savedPath -PathType Leaf)) {
        throw 'System-profile export did not create a file.'
    }

    $roundTrip = Get-Content -LiteralPath $savedPath -Raw | ConvertFrom-Json
    if ($roundTrip.SchemaVersion -ne 1) {
        throw 'System-profile JSON did not round-trip correctly.'
    }
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'Unified setup inventory and profile-schema validation passed.' -ForegroundColor Green
