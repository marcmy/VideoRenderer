#requires -Version 5.1

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$commonModule = Join-Path $PSScriptRoot 'MpcvrSetup.Common.psm1'
$transactionModule = Join-Path $PSScriptRoot 'MpcvrSetup.Transaction.psm1'
$profilesModule = Join-Path $PSScriptRoot 'MpcvrSetup.Profiles.psm1'
$schemaPath = Join-Path $PSScriptRoot 'profiles\profile.schema.json'
$installerPath = Join-Path $PSScriptRoot 'Install-MpcvrUnified.ps1'
$restorePath = Join-Path $PSScriptRoot 'Restore-MpcvrUnifiedBackup.ps1'
$profileManagerPath = Join-Path $PSScriptRoot 'Manage-MpcvrProfiles.ps1'
$telemetryReaderPath = Join-Path $PSScriptRoot 'Get-MpcvrRendererTelemetry.ps1'
$frucInstallerPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'nvoffruc\Install-NvOFFRUCRuntime.ps1'

$requiredFiles = @(
    $commonModule,
    $transactionModule,
    $profilesModule,
    $schemaPath,
    $installerPath,
    $restorePath,
    $profileManagerPath,
    $telemetryReaderPath,
    $frucInstallerPath
)
foreach ($path in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing unified setup file: $path"
    }
}

Import-Module -Name $commonModule -Force
Import-Module -Name $transactionModule -Force
Import-Module -Name $profilesModule -Force

$requiredCommands = @(
    'Get-MpcvrGpuProfile', 'Get-MpcvrDisplayProfile', 'Get-MpcvrRuntimeStatus',
    'Get-MpcvrPlayerTargets', 'Get-MpcvrSystemProfile', 'Save-MpcvrSystemProfile',
    'Test-MpcvrAdministrator', 'Get-MpcvrPowerShellExecutable',
    'ConvertTo-MpcvrCommandLineArgument', 'Invoke-MpcvrPowerShellScript',
    'Get-MpcvrSnapshotItemsFromProfile', 'New-MpcvrSetupSnapshot',
    'Restore-MpcvrSetupSnapshot', 'Test-MpcvrPowerShellScriptSyntax',
    'Get-MpcvrProfileRoot', 'Get-MpcvrProfilePath', 'New-MpcvrProfile',
    'Test-MpcvrProfile', 'Read-MpcvrProfile', 'Save-MpcvrProfile',
    'Get-MpcvrProfiles', 'Find-MpcvrProfile', 'Set-MpcvrProfileLock',
    'Remove-MpcvrProfile', 'Export-MpcvrProfile', 'Import-MpcvrProfile',
    'Restore-MpcvrFactoryProfiles'
)
foreach ($commandName in $requiredCommands) {
    if (-not (Get-Command -Name $commandName -ErrorAction SilentlyContinue)) {
        throw "The unified setup modules do not export $commandName."
    }
}

foreach ($scriptPath in @(
    $commonModule, $transactionModule, $profilesModule, $installerPath,
    $restorePath, $profileManagerPath, $telemetryReaderPath, $frucInstallerPath
)) {
    [void](Test-MpcvrPowerShellScriptSyntax -ScriptPath $scriptPath)
}

$schema = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json
if ($schema.title -ne 'MPCVR Unified Setup Profile' -or
    $schema.properties.mode.enum.Count -ne 3 -or
    $schema.properties.locked.type -ne 'boolean') {
    throw 'The portable profile schema is incomplete.'
}

$systemProfile = Get-MpcvrSystemProfile
if ($systemProfile.SchemaVersion -ne 1 -or
    $null -eq $systemProfile.Gpus -or
    $null -eq $systemProfile.Runtimes -or
    $null -eq $systemProfile.Players) {
    throw 'The system profile is incomplete.'
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('MPCVR-UnifiedSetup-Test-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

    # Inventory JSON round trip.
    $systemProfilePath = Save-MpcvrSystemProfile `
        -Profile $systemProfile `
        -Path (Join-Path $tempRoot 'system-profile.json')
    $roundTrip = Get-Content -LiteralPath $systemProfilePath -Raw | ConvertFrom-Json
    if ($roundTrip.SchemaVersion -ne 1) {
        throw 'System-profile JSON did not round-trip correctly.'
    }

    # Disposable transaction and rollback test.
    $stateRoot = Join-Path $tempRoot 'state'
    $runtimeRoot = Join-Path $stateRoot 'runtime'
    $rendererPath = Join-Path $stateRoot 'renderer\MpcVideoRenderer64.ax'
    $newPath = Join-Path $stateRoot 'new-runtime'
    New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($rendererPath)) -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $runtimeRoot 'runtime.dll') -Value 'runtime-before' -Encoding UTF8
    Set-Content -LiteralPath $rendererPath -Value 'renderer-before' -Encoding UTF8

    $snapshotRoot = Join-Path $tempRoot 'snapshot'
    [void](New-MpcvrSetupSnapshot `
        -SnapshotRoot $snapshotRoot `
        -Items @(
            [pscustomobject]@{ Name = 'Test runtime'; Kind = 'Directory'; Path = $runtimeRoot },
            [pscustomobject]@{ Name = 'Test renderer'; Kind = 'File'; Path = $rendererPath },
            [pscustomobject]@{ Name = 'New runtime'; Kind = 'Directory'; Path = $newPath }
        ) `
        -EnvironmentValues @{ NV_VIDEO_EFFECTS_PATH = 'test-maxine'; NV_OFFRUC_PATH = 'test-fruc' })

    Set-Content -LiteralPath (Join-Path $runtimeRoot 'runtime.dll') -Value 'runtime-after' -Encoding UTF8
    Set-Content -LiteralPath $rendererPath -Value 'renderer-after' -Encoding UTF8
    New-Item -ItemType Directory -Path $newPath -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $newPath 'new.dll') -Value 'new' -Encoding UTF8

    [void](Restore-MpcvrSetupSnapshot -SnapshotRoot $snapshotRoot -SkipEnvironmentRestore)
    if ((Get-Content -LiteralPath (Join-Path $runtimeRoot 'runtime.dll') -Raw).Trim() -ne 'runtime-before' -or
        (Get-Content -LiteralPath $rendererPath -Raw).Trim() -ne 'renderer-before' -or
        (Test-Path -LiteralPath $newPath)) {
        throw 'Transactional rollback did not reproduce the original filesystem state.'
    }
    $manifest = Get-Content -LiteralPath (Join-Path $snapshotRoot 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.State -ne 'Restored' -or [string]::IsNullOrWhiteSpace([string]$manifest.RestoredAtUtc)) {
        throw 'Rollback did not mark the snapshot as restored.'
    }

    # Profile lifecycle and lock protection.
    $profileRoot = Join-Path $tempRoot 'profiles'
    $factoryPaths = @(Restore-MpcvrFactoryProfiles -ProfileRoot $profileRoot)
    $storedProfiles = @(Get-MpcvrProfiles -ProfileRoot $profileRoot)
    if ($factoryPaths.Count -ne 2 -or $storedProfiles.Count -ne 2 -or
        @($storedProfiles | Where-Object { -not $_.Valid }).Count -ne 0) {
        throw 'Factory profiles were not stored and validated correctly.'
    }
    foreach ($stored in $storedProfiles) {
        if ([bool]$stored.Profile.maxine.enabled -or [bool]$stored.Profile.frameInterpolation.enabled) {
            throw 'Uncalibrated factory profiles must not enable GPU processing automatically.'
        }
    }

    $advanced = New-MpcvrProfile `
        -Name 'Advanced Test Profile' `
        -Mode Advanced `
        -SourceFrameRateClass custom `
        -Locked
    $advanced.maxine.enabled = $true
    $advanced.maxine.quality = 'custom'
    $advanced.maxine.scaleLimit = 3.25
    $advanced.maxine | Add-Member -NotePropertyName customOption -NotePropertyValue 'preserved'
    $advanced.frameInterpolation.enabled = $true
    $advanced.frameInterpolation.sourceResolutionLimit = 'custom'
    $advanced.frameInterpolation.maxOutputFps = 144.0
    $advanced.frameInterpolation | Add-Member -NotePropertyName customOption -NotePropertyValue 42
    $advanced.fallback.strategy = 'none'
    $advanced.fallback.showWarning = $false

    $advancedPath = Save-MpcvrProfile -Profile $advanced -ProfileRoot $profileRoot
    $advancedRoundTrip = Read-MpcvrProfile -Path $advancedPath
    if (-not [bool]$advancedRoundTrip.locked -or
        [string]$advancedRoundTrip.maxine.customOption -ne 'preserved' -or
        [int]$advancedRoundTrip.frameInterpolation.customOption -ne 42) {
        throw 'Advanced profile round-trip did not preserve lock state or custom properties.'
    }

    $overwriteBlocked = $false
    try {
        $advancedRoundTrip.maxine.scaleLimit = 2.5
        [void](Save-MpcvrProfile -Profile $advancedRoundTrip -ProfileRoot $profileRoot -Force)
    }
    catch {
        $overwriteBlocked = $_.Exception.Message -match 'locked'
    }
    if (-not $overwriteBlocked) {
        throw 'A locked profile was overwritten without explicit permission.'
    }

    [void](Set-MpcvrProfileLock -Name 'Advanced Test Profile' -Locked $false -ProfileRoot $profileRoot)
    $unlocked = (Find-MpcvrProfile -Name 'Advanced Test Profile' -ProfileRoot $profileRoot).Profile
    $unlocked.maxine.scaleLimit = 2.5
    [void](Save-MpcvrProfile -Profile $unlocked -ProfileRoot $profileRoot -Force)
    [void](Set-MpcvrProfileLock -Name 'Advanced Test Profile' -Locked $true -ProfileRoot $profileRoot)

    $exportPath = Join-Path $tempRoot 'exports\advanced.json'
    [void](Export-MpcvrProfile `
        -Name 'Advanced Test Profile' `
        -Destination $exportPath `
        -ProfileRoot $profileRoot)
    $importedPath = Import-MpcvrProfile `
        -Source $exportPath `
        -Name 'Imported Advanced Profile' `
        -ProfileRoot $profileRoot
    $imported = Read-MpcvrProfile -Path $importedPath
    if ($imported.name -ne 'Imported Advanced Profile' -or -not [bool]$imported.locked) {
        throw 'Profile import did not preserve content or apply the requested name.'
    }

    $deleteBlocked = $false
    try {
        Remove-MpcvrProfile -Name 'Imported Advanced Profile' -ProfileRoot $profileRoot
    }
    catch {
        $deleteBlocked = $_.Exception.Message -match 'locked'
    }
    if (-not $deleteBlocked) {
        throw 'A locked profile was deleted without explicit permission.'
    }
    Remove-MpcvrProfile `
        -Name 'Imported Advanced Profile' `
        -ProfileRoot $profileRoot `
        -AllowLockedDelete

    $protectedFactoryName = 'Automatic - up to 30 fps (uncalibrated)'
    [void](Set-MpcvrProfileLock -Name $protectedFactoryName -Locked $true -ProfileRoot $profileRoot)
    [void](Restore-MpcvrFactoryProfiles -ProfileRoot $profileRoot -Force)
    if (-not [bool](Find-MpcvrProfile -Name $protectedFactoryName -ProfileRoot $profileRoot).Profile.locked) {
        throw 'Factory restore overwrote a manually locked profile.'
    }

    Invoke-MpcvrPowerShellScript `
        -Name 'Validating profile manager list action...' `
        -ScriptPath $profileManagerPath `
        -Arguments @('-Action', 'List', '-ProfileRoot', (ConvertTo-MpcvrCommandLineArgument -Value $profileRoot), '-Json') `
        -NoNewWindow
    Invoke-MpcvrPowerShellScript `
        -Name 'Validating profile manager duplicate action...' `
        -ScriptPath $profileManagerPath `
        -Arguments @(
            '-Action', 'Duplicate',
            '-Name', (ConvertTo-MpcvrCommandLineArgument -Value 'Advanced Test Profile'),
            '-NewName', (ConvertTo-MpcvrCommandLineArgument -Value 'CLI Duplicate'),
            '-ProfileRoot', (ConvertTo-MpcvrCommandLineArgument -Value $profileRoot)
        ) `
        -NoNewWindow
    $duplicate = (Find-MpcvrProfile -Name 'CLI Duplicate' -ProfileRoot $profileRoot).Profile
    if ([bool]$duplicate.locked -or $duplicate.mode -ne 'advanced') {
        throw 'Profile-manager duplication did not create an unlocked editable copy.'
    }

    # Entry-point and shared-memory reader validation.
    Invoke-MpcvrPowerShellScript `
        -Name 'Validating renderer telemetry reader...' `
        -ScriptPath $telemetryReaderPath `
        -Arguments @('-ValidateOnly') `
        -NoNewWindow
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

Write-Host 'Unified setup inventory, transaction, rollback, profiles, and telemetry validation passed.' -ForegroundColor Green
