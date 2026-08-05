#requires -Version 5.1

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$commonModule = Join-Path $PSScriptRoot 'MpcvrSetup.Common.psm1'
$transactionModule = Join-Path $PSScriptRoot 'MpcvrSetup.Transaction.psm1'
$schemaPath = Join-Path $PSScriptRoot 'profiles\profile.schema.json'
$installerPath = Join-Path $PSScriptRoot 'Install-MpcvrUnified.ps1'
$restorePath = Join-Path $PSScriptRoot 'Restore-MpcvrUnifiedBackup.ps1'
$frucInstallerPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'nvoffruc\Install-NvOFFRUCRuntime.ps1'

foreach ($path in @($commonModule, $transactionModule, $schemaPath, $installerPath, $restorePath, $frucInstallerPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing unified setup file: $path"
    }
}

Import-Module -Name $commonModule -Force
Import-Module -Name $transactionModule -Force

$requiredCommands = @(
    'Get-MpcvrGpuProfile',
    'Get-MpcvrDisplayProfile',
    'Get-MpcvrRuntimeStatus',
    'Get-MpcvrPlayerTargets',
    'Get-MpcvrSystemProfile',
    'Save-MpcvrSystemProfile',
    'Test-MpcvrAdministrator',
    'Get-MpcvrPowerShellExecutable',
    'ConvertTo-MpcvrCommandLineArgument',
    'Invoke-MpcvrPowerShellScript',
    'Get-MpcvrSnapshotItemsFromProfile',
    'New-MpcvrSetupSnapshot',
    'Restore-MpcvrSetupSnapshot',
    'Test-MpcvrPowerShellScriptSyntax'
)

foreach ($commandName in $requiredCommands) {
    if (-not (Get-Command -Name $commandName -ErrorAction SilentlyContinue)) {
        throw "The unified setup modules do not export $commandName."
    }
}

foreach ($scriptPath in @($commonModule, $transactionModule, $installerPath, $restorePath, $frucInstallerPath)) {
    [void](Test-MpcvrPowerShellScriptSyntax -ScriptPath $scriptPath)
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
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

    $outputPath = Join-Path $tempRoot 'system-profile.json'
    $savedPath = Save-MpcvrSystemProfile -Profile $profile -Path $outputPath
    if (-not (Test-Path -LiteralPath $savedPath -PathType Leaf)) {
        throw 'System-profile export did not create a file.'
    }

    $roundTrip = Get-Content -LiteralPath $savedPath -Raw | ConvertFrom-Json
    if ($roundTrip.SchemaVersion -ne 1) {
        throw 'System-profile JSON did not round-trip correctly.'
    }

    $stateRoot = Join-Path $tempRoot 'state'
    $runtimeRoot = Join-Path $stateRoot 'runtime'
    $rendererPath = Join-Path $stateRoot 'renderer\MpcVideoRenderer64.ax'
    $newPath = Join-Path $stateRoot 'new-runtime'
    New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($rendererPath)) -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $runtimeRoot 'runtime.dll') -Value 'runtime-before' -Encoding UTF8
    Set-Content -LiteralPath $rendererPath -Value 'renderer-before' -Encoding UTF8

    $items = @(
        [pscustomobject]@{ Name = 'Test runtime'; Kind = 'Directory'; Path = $runtimeRoot },
        [pscustomobject]@{ Name = 'Test renderer'; Kind = 'File'; Path = $rendererPath },
        [pscustomobject]@{ Name = 'New runtime'; Kind = 'Directory'; Path = $newPath }
    )
    $snapshotRoot = Join-Path $tempRoot 'snapshot'
    [void](New-MpcvrSetupSnapshot `
        -SnapshotRoot $snapshotRoot `
        -Items $items `
        -EnvironmentValues @{ NV_VIDEO_EFFECTS_PATH = 'test-maxine'; NV_OFFRUC_PATH = 'test-fruc' })

    Set-Content -LiteralPath (Join-Path $runtimeRoot 'runtime.dll') -Value 'runtime-after' -Encoding UTF8
    Set-Content -LiteralPath $rendererPath -Value 'renderer-after' -Encoding UTF8
    New-Item -ItemType Directory -Path $newPath -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $newPath 'new.dll') -Value 'new' -Encoding UTF8

    [void](Restore-MpcvrSetupSnapshot -SnapshotRoot $snapshotRoot -SkipEnvironmentRestore)

    $runtimeText = (Get-Content -LiteralPath (Join-Path $runtimeRoot 'runtime.dll') -Raw).Trim()
    $rendererText = (Get-Content -LiteralPath $rendererPath -Raw).Trim()
    if ($runtimeText -ne 'runtime-before') {
        throw 'Directory rollback did not restore the original runtime content.'
    }
    if ($rendererText -ne 'renderer-before') {
        throw 'File rollback did not restore the original renderer content.'
    }
    if (Test-Path -LiteralPath $newPath) {
        throw 'Rollback did not remove a path that was absent before the transaction.'
    }

    $manifest = Get-Content -LiteralPath (Join-Path $snapshotRoot 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.State -ne 'Restored' -or [string]::IsNullOrWhiteSpace([string]$manifest.RestoredAtUtc)) {
        throw 'Rollback did not mark the snapshot as restored.'
    }

    Invoke-MpcvrPowerShellScript `
        -Name 'Validating unified installer entry point...' `
        -ScriptPath $installerPath `
        -Arguments @('-ValidateOnly', '-NoPause') `
        -NoNewWindow
    Invoke-MpcvrPowerShellScript `
        -Name 'Validating unified rollback entry point...' `
        -ScriptPath $restorePath `
        -Arguments @('-ValidateOnly', '-NoPause') `
        -NoNewWindow
    Invoke-MpcvrPowerShellScript `
        -Name 'Validating NvOFFRUC runtime installer entry point...' `
        -ScriptPath $frucInstallerPath `
        -Arguments @('-ValidateOnly', '-NoPause') `
        -NoNewWindow
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'Unified setup inventory, profile, transaction, and rollback validation passed.' -ForegroundColor Green
