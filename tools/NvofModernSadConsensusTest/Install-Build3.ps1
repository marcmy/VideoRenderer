#requires -Version 5.1

[CmdletBinding()]
param(
    [switch]$Restore,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$targets = [ordered]@{
    'MpcVideoRenderer.ax'   = 'C:\Program Files (x86)\K-Lite Codec Pack\Filters\MPCVR\MpcVideoRenderer.ax'
    'MpcVideoRenderer64.ax' = 'C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax'
}

$payloadRoot = Join-Path $PSScriptRoot 'payload'
$backupRoot = Join-Path $env:ProgramData 'MPCVR-Modern-SAD-Consensus-Build3-Backup'
$manifestPath = Join-Path $backupRoot 'backup-manifest.txt'

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-PowerShellExecutable {
    $pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
    if ($pwsh) { return $pwsh.Source }
    $powershell = Get-Command powershell.exe -ErrorAction SilentlyContinue
    if ($powershell) { return $powershell.Source }
    throw 'Neither pwsh.exe nor powershell.exe was found.'
}

function Quote-Arg([string]$Value) {
    return '"{0}"' -f $Value.Replace('"', '\"')
}

function Finish([int]$ExitCode) {
    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'This test installer only supports Windows.' -ForegroundColor Red
    Finish 1
}

if (-not (Test-IsAdministrator)) {
    $exe = Get-PowerShellExecutable
    $args = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File {0}' -f (Quote-Arg $PSCommandPath)
    if ($Restore) { $args += ' -Restore' }
    if ($NoPause) { $args += ' -NoPause' }
    try {
        $process = Start-Process -FilePath $exe -ArgumentList $args -Verb RunAs -Wait -PassThru
        exit $process.ExitCode
    }
    catch {
        Write-Host "Administrator elevation was cancelled or failed: $($_.Exception.Message)" -ForegroundColor Red
        Finish 1
    }
}

try {
    if (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
        throw 'Close MPC-HC before installing or restoring MPC Video Renderer.'
    }

    foreach ($destination in $targets.Values) {
        $directory = [IO.Path]::GetDirectoryName($destination)
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            throw "K-Lite destination directory was not found: $directory"
        }
    }

    if ($Restore) {
        if (-not (Test-Path -LiteralPath $backupRoot -PathType Container)) {
            throw "No Build #3 backup exists at $backupRoot"
        }
        foreach ($fileName in $targets.Keys) {
            $backup = Join-Path $backupRoot $fileName
            $destination = $targets[$fileName]
            if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
                throw "Backup is incomplete; missing $backup"
            }
            Write-Host "Restoring $fileName..."
            Copy-Item -LiteralPath $backup -Destination $destination -Force
            if ((Get-FileHash $backup -Algorithm SHA256).Hash -ne
                (Get-FileHash $destination -Algorithm SHA256).Hash) {
                throw "Hash verification failed while restoring $fileName."
            }
        }
        Write-Host
        Write-Host 'Pre-Build-3 MPC Video Renderer binaries restored successfully.' -ForegroundColor Green
        Finish 0
    }

    foreach ($fileName in $targets.Keys) {
        $payload = Join-Path $payloadRoot $fileName
        if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) {
            throw "Test package is incomplete; missing payload\$fileName"
        }
    }

    # Preserve the renderer installed before the first Build #3 test and never
    # overwrite that rollback point on later iterations.
    if (-not (Test-Path -LiteralPath $backupRoot -PathType Container)) {
        New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
        $manifest = @(
            'MPCVR Build #3 modern-SAD consensus pre-test backup'
            "Created: $(Get-Date -Format o)"
        )
        foreach ($fileName in $targets.Keys) {
            $destination = $targets[$fileName]
            if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
                throw "Installed K-Lite renderer was not found: $destination"
            }
            $backup = Join-Path $backupRoot $fileName
            Copy-Item -LiteralPath $destination -Destination $backup -Force
            $manifest += "$fileName $((Get-FileHash $backup -Algorithm SHA256).Hash)"
        }
        [IO.File]::WriteAllLines($manifestPath, $manifest, [Text.UTF8Encoding]::new($false))
        Write-Host "Saved rollback copy to $backupRoot"
    }
    else {
        foreach ($fileName in $targets.Keys) {
            if (-not (Test-Path -LiteralPath (Join-Path $backupRoot $fileName) -PathType Leaf)) {
                throw "Existing rollback directory is incomplete: $backupRoot"
            }
        }
        Write-Host "Keeping existing rollback copy at $backupRoot"
    }

    foreach ($fileName in $targets.Keys) {
        $source = Join-Path $payloadRoot $fileName
        $destination = $targets[$fileName]
        Write-Host "Installing Build #3 experimental $fileName..."
        Copy-Item -LiteralPath $source -Destination $destination -Force
        if ((Get-FileHash $source -Algorithm SHA256).Hash -ne
            (Get-FileHash $destination -Algorithm SHA256).Hash) {
            throw "Hash verification failed after copying $fileName."
        }
    }

    Write-Host
    Write-Host 'MPCVR Build #3 modern-SAD consensus test installed successfully.' -ForegroundColor Green
    Write-Host 'No registration changes were made. Restart MPC-HC and test normally.'
    Write-Host 'Use Restore-Golden.cmd to restore the renderer installed before this test.'
    Finish 0
}
catch {
    Write-Host
    Write-Host "Build #3 installer failed: $($_.Exception.Message)" -ForegroundColor Red
    Finish 1
}
