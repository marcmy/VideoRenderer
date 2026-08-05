#requires -Version 5.1

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Test-MpcvrAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal -ArgumentList $identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-MpcvrPowerShellExecutable {
    $pwsh = Get-Command 'pwsh.exe' -ErrorAction SilentlyContinue
    if ($null -ne $pwsh) {
        return $pwsh.Source
    }

    $windowsPowerShell = Get-Command 'powershell.exe' -ErrorAction SilentlyContinue
    if ($null -ne $windowsPowerShell) {
        return $windowsPowerShell.Source
    }

    throw 'Neither PowerShell 7 nor Windows PowerShell 5.1 was found.'
}

function ConvertTo-MpcvrCommandLineArgument {
    param(
        [AllowEmptyString()]
        [string]$Value
    )

    if ($null -eq $Value) {
        return '""'
    }

    return '"{0}"' -f $Value.Replace('"', '\"')
}

function Invoke-MpcvrPowerShellScript {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [Parameter(Mandatory)]
        [string]$ScriptPath,
        [string[]]$Arguments = @(),
        [switch]$NoNewWindow
    )

    $resolvedScript = (Resolve-Path -LiteralPath $ScriptPath).Path
    $argumentParts = @(
        '-NoLogo',
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        (ConvertTo-MpcvrCommandLineArgument -Value $resolvedScript)
    ) + @($Arguments)

    Write-Host
    Write-Host $Name -ForegroundColor Cyan

    $startParameters = @{
        FilePath = Get-MpcvrPowerShellExecutable
        ArgumentList = ($argumentParts -join ' ')
        Wait = $true
        PassThru = $true
    }
    if ($NoNewWindow) {
        $startParameters.NoNewWindow = $true
    }

    $process = Start-Process @startParameters
    if ($process.ExitCode -ne 0) {
        throw "$Name failed with exit code $($process.ExitCode)."
    }
}

function Test-MpcvrSafeStatePath {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($fullPath)
    if ([string]::IsNullOrWhiteSpace($root) -or $fullPath.TrimEnd('\') -ieq $root.TrimEnd('\')) {
        throw "Refusing to snapshot or restore an unsafe root path: $fullPath"
    }
    return $fullPath
}

function Get-MpcvrSnapshotItemsFromProfile {
    param(
        [Parameter(Mandatory)]
        [object]$Profile
    )

    $items = New-Object 'System.Collections.Generic.List[object]'
    $seen = @{}

    $candidates = @(
        [pscustomobject]@{ Name = 'Maxine runtime'; Kind = 'Directory'; Path = [string]$Profile.Runtimes.Maxine.Path },
        [pscustomobject]@{ Name = 'NvOFFRUC runtime'; Kind = 'Directory'; Path = [string]$Profile.Runtimes.NvOFFRUC.Path }
    )

    foreach ($player in @($Profile.Players)) {
        $candidates += [pscustomobject]@{
            Name = [string]$player.Name
            Kind = 'File'
            Path = [string]$player.RendererPath
        }
    }

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate.Path)) {
            continue
        }
        $fullPath = Test-MpcvrSafeStatePath -Path $candidate.Path
        if (-not $seen.ContainsKey($fullPath)) {
            $seen[$fullPath] = $true
            $items.Add([pscustomobject]@{
                Name = $candidate.Name
                Kind = $candidate.Kind
                Path = $fullPath
            })
        }
    }

    return @($items)
}

function New-MpcvrSetupSnapshot {
    param(
        [Parameter(Mandatory)]
        [string]$SnapshotRoot,
        [Parameter(Mandatory)]
        [object[]]$Items,
        [hashtable]$EnvironmentValues
    )

    $root = [IO.Path]::GetFullPath($SnapshotRoot)
    if (Test-Path -LiteralPath $root) {
        throw "The snapshot destination already exists: $root"
    }

    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $itemsRoot = Join-Path $root 'items'
    New-Item -ItemType Directory -Path $itemsRoot -Force | Out-Null

    if ($null -eq $EnvironmentValues) {
        $EnvironmentValues = @{
            NV_VIDEO_EFFECTS_PATH = [Environment]::GetEnvironmentVariable('NV_VIDEO_EFFECTS_PATH', 'User')
            NV_OFFRUC_PATH = [Environment]::GetEnvironmentVariable('NV_OFFRUC_PATH', 'User')
        }
    }

    $manifestItems = New-Object 'System.Collections.Generic.List[object]'
    $index = 0
    foreach ($item in @($Items)) {
        $path = Test-MpcvrSafeStatePath -Path ([string]$item.Path)
        $kind = [string]$item.Kind
        if ($kind -notin @('File', 'Directory')) {
            throw "Unsupported snapshot item kind '$kind' for $path."
        }

        $exists = if ($kind -eq 'File') {
            Test-Path -LiteralPath $path -PathType Leaf
        }
        else {
            Test-Path -LiteralPath $path -PathType Container
        }

        $relativeBackup = $null
        $hash = $null
        if ($exists) {
            $relativeBackup = 'items/{0:D3}' -f $index
            $backupPath = Join-Path $root ($relativeBackup -replace '/', '\')
            if ($kind -eq 'File') {
                New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($backupPath)) -Force | Out-Null
                Copy-Item -LiteralPath $path -Destination $backupPath -Force
                $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            }
            else {
                Copy-Item -LiteralPath $path -Destination $backupPath -Recurse -Force
            }
        }

        $manifestItems.Add([pscustomobject]@{
            Name = [string]$item.Name
            Kind = $kind
            OriginalPath = $path
            Existed = [bool]$exists
            BackupRelativePath = $relativeBackup
            Sha256 = $hash
        })
        $index++
    }

    $manifest = [pscustomobject]@{
        SchemaVersion = 1
        State = 'Created'
        CreatedAtUtc = [DateTime]::UtcNow.ToString('o')
        RestoredAtUtc = $null
        Environment = [pscustomobject]@{
            NV_VIDEO_EFFECTS_PATH = $EnvironmentValues['NV_VIDEO_EFFECTS_PATH']
            NV_OFFRUC_PATH = $EnvironmentValues['NV_OFFRUC_PATH']
        }
        Items = @($manifestItems)
    }

    $manifestPath = Join-Path $root 'manifest.json'
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    return $root
}

function Publish-MpcvrEnvironmentChange {
    try {
        if (-not ('MpcvrUnifiedSetup.EnvironmentBroadcast' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace MpcvrUnifiedSetup {
    public static class EnvironmentBroadcast {
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr SendMessageTimeout(
            IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
            uint flags, uint timeout, out UIntPtr result);
    }
}
'@
        }
        $result = [UIntPtr]::Zero
        [void][MpcvrUnifiedSetup.EnvironmentBroadcast]::SendMessageTimeout(
            [IntPtr]0xffff, 0x001A, [UIntPtr]::Zero, 'Environment',
            2, 5000, [ref]$result)
    }
    catch {
        Write-Warning "Environment variables were restored, but the live refresh broadcast failed: $($_.Exception.Message)"
    }
}

function Restore-MpcvrSetupSnapshot {
    param(
        [Parameter(Mandatory)]
        [string]$SnapshotRoot,
        [switch]$SkipEnvironmentRestore
    )

    $root = (Resolve-Path -LiteralPath $SnapshotRoot).Path
    $manifestPath = Join-Path $root 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "The snapshot manifest is missing: $manifestPath"
    }

    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.SchemaVersion -ne 1) {
        throw "Unsupported snapshot schema version: $($manifest.SchemaVersion)"
    }

    $restoreItems = @($manifest.Items)
    [array]::Reverse($restoreItems)
    foreach ($item in $restoreItems) {
        $path = Test-MpcvrSafeStatePath -Path ([string]$item.OriginalPath)
        if ([bool]$item.Existed) {
            if ([string]::IsNullOrWhiteSpace([string]$item.BackupRelativePath)) {
                throw "Snapshot item '$($item.Name)' has no backup path."
            }
            $backupPath = Join-Path $root (([string]$item.BackupRelativePath) -replace '/', '\')
            if (-not (Test-Path -LiteralPath $backupPath)) {
                throw "Snapshot payload is missing for '$($item.Name)': $backupPath"
            }

            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path)) -Force | Out-Null
            if ([string]$item.Kind -eq 'File') {
                Copy-Item -LiteralPath $backupPath -Destination $path -Force
            }
            else {
                Copy-Item -LiteralPath $backupPath -Destination $path -Recurse -Force
            }

            if ([string]$item.Kind -eq 'File' -and -not [string]::IsNullOrWhiteSpace([string]$item.Sha256)) {
                $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($actualHash -ne ([string]$item.Sha256).ToLowerInvariant()) {
                    throw "Restored file verification failed for $path."
                }
            }
        }
        else {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    if (-not $SkipEnvironmentRestore) {
        $maxineValue = $manifest.Environment.NV_VIDEO_EFFECTS_PATH
        $frucValue = $manifest.Environment.NV_OFFRUC_PATH
        [Environment]::SetEnvironmentVariable('NV_VIDEO_EFFECTS_PATH', $maxineValue, 'User')
        [Environment]::SetEnvironmentVariable('NV_OFFRUC_PATH', $frucValue, 'User')
        $env:NV_VIDEO_EFFECTS_PATH = $maxineValue
        $env:NV_OFFRUC_PATH = $frucValue
        Publish-MpcvrEnvironmentChange
    }

    $manifest.State = 'Restored'
    $manifest.RestoredAtUtc = [DateTime]::UtcNow.ToString('o')
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    return $root
}

function Test-MpcvrPowerShellScriptSyntax {
    param(
        [Parameter(Mandatory)]
        [string]$ScriptPath
    )

    $resolved = (Resolve-Path -LiteralPath $ScriptPath).Path
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($resolved, [ref]$tokens, [ref]$errors)
    if (@($errors).Count -gt 0) {
        $messages = @($errors | ForEach-Object { $_.Message }) -join '; '
        throw "PowerShell syntax validation failed for $resolved: $messages"
    }
    return $true
}

Export-ModuleMember -Function @(
    'Test-MpcvrAdministrator',
    'Get-MpcvrPowerShellExecutable',
    'ConvertTo-MpcvrCommandLineArgument',
    'Invoke-MpcvrPowerShellScript',
    'Get-MpcvrSnapshotItemsFromProfile',
    'New-MpcvrSetupSnapshot',
    'Restore-MpcvrSetupSnapshot',
    'Test-MpcvrPowerShellScriptSyntax'
)
