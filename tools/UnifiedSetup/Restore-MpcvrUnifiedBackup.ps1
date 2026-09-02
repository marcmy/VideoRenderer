#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$BackupPath,
    [string]$DataRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup'),
    [switch]$ValidateOnly,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$commonModule = Join-Path $PSScriptRoot 'MpcvrSetup.Common.psm1'
$transactionModule = Join-Path $PSScriptRoot 'MpcvrSetup.Transaction.psm1'
Import-Module -Name $commonModule -Force
Import-Module -Name $transactionModule -Force

function Complete-Run {
    param([int]$ExitCode)

    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

function Resolve-BackupPath {
    if (-not [string]::IsNullOrWhiteSpace($BackupPath)) {
        return (Resolve-Path -LiteralPath $BackupPath).Path
    }

    $pointerPath = Join-Path ([IO.Path]::GetFullPath($DataRoot)) 'latest-backup.txt'
    if (-not (Test-Path -LiteralPath $pointerPath -PathType Leaf)) {
        throw "No latest rollback pointer was found: $pointerPath"
    }

    $latest = (Get-Content -LiteralPath $pointerPath -Raw).Trim()
    if ([string]::IsNullOrWhiteSpace($latest)) {
        throw "The latest rollback pointer is empty: $pointerPath"
    }
    return (Resolve-Path -LiteralPath $latest).Path
}

function Start-ElevatedSelf {
    $parts = @(
        '-NoLogo',
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        (ConvertTo-MpcvrCommandLineArgument -Value $PSCommandPath),
        '-BackupPath',
        (ConvertTo-MpcvrCommandLineArgument -Value $resolvedBackup),
        '-DataRoot',
        (ConvertTo-MpcvrCommandLineArgument -Value ([IO.Path]::GetFullPath($DataRoot)))
    )
    if ($NoPause) {
        $parts += '-NoPause'
    }

    try {
        $process = Start-Process `
            -FilePath (Get-MpcvrPowerShellExecutable) `
            -ArgumentList ($parts -join ' ') `
            -Verb RunAs `
            -Wait `
            -PassThru
        exit $process.ExitCode
    }
    catch {
        throw "Administrator elevation was cancelled or failed: $($_.Exception.Message)"
    }
}

$exitCode = 0
try {
    foreach ($path in @($commonModule, $transactionModule)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Unified rollback is incomplete. Missing: $path"
        }
        [void](Test-MpcvrPowerShellScriptSyntax -ScriptPath $path)
    }

    if ($ValidateOnly) {
        Write-Host 'Unified rollback validation passed.' -ForegroundColor Green
        Complete-Run -ExitCode 0
    }

    $resolvedBackup = Resolve-BackupPath
    $manifestPath = Join-Path $resolvedBackup 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "The selected rollback snapshot has no manifest: $manifestPath"
    }

    if (-not (Test-MpcvrAdministrator)) {
        Start-ElevatedSelf
    }

    if (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
        throw 'Close MPC-HC before restoring an MPCVR Unified Setup backup.'
    }

    [void](Restore-MpcvrSetupSnapshot -SnapshotRoot $resolvedBackup)
    $restoredProfile = Get-MpcvrSystemProfile
    [void](Save-MpcvrSystemProfile -Profile $restoredProfile -Path (Join-Path $resolvedBackup 'system-restored.json'))

    $transactionPath = Join-Path $resolvedBackup 'transaction.json'
    if (Test-Path -LiteralPath $transactionPath -PathType Leaf) {
        $transaction = Get-Content -LiteralPath $transactionPath -Raw | ConvertFrom-Json
        $transaction.Status = 'RestoredByUser'
        $transaction.CompletedAtUtc = [DateTime]::UtcNow.ToString('o')
        $transaction | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $transactionPath -Encoding UTF8
    }

    Write-Host
    Write-Host 'MPCVR renderer and NVIDIA runtime state restored successfully.' -ForegroundColor Green
    Write-Host "Restored snapshot: $resolvedBackup"
    Write-Host 'Restart MPC-HC before testing playback.'
}
catch {
    $exitCode = 1
    Write-Host
    Write-Host "Rollback failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}

Complete-Run -ExitCode $exitCode
