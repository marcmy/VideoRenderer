#requires -Version 5.1

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Get-MpcvrKnownPlayerPaths {
    return @(
        'C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\mpc-hc64.exe',
        'C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC\mpc-hc.exe',
        'C:\Program Files\MPC-HC\mpc-hc64.exe',
        'C:\Program Files (x86)\MPC-HC\mpc-hc.exe'
    )
}

function Resolve-MpcvrPlayerPath {
    param([string]$PlayerPath)

    if (-not [string]::IsNullOrWhiteSpace($PlayerPath)) {
        $resolved = (Resolve-Path -LiteralPath $PlayerPath).Path
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "MPC-HC executable was not found: $resolved"
        }
        return $resolved
    }

    foreach ($candidate in Get-MpcvrKnownPlayerPaths) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw 'MPC-HC was not found in a supported K-Lite or standalone location. Supply PlayerPath explicitly.'
}

function Get-MpcvrSettingsSignature {
    param(
        [Parameter(Mandatory)]
        [object]$Settings
    )

    $pairs = @()
    if ($Settings -is [System.Collections.IDictionary]) {
        foreach ($key in @($Settings.Keys | Sort-Object)) {
            $pairs += ('{0}={1}' -f $key, [int64]$Settings[$key])
        }
    }
    else {
        foreach ($property in @($Settings.PSObject.Properties | Sort-Object Name)) {
            $pairs += ('{0}={1}' -f $property.Name, [int64]$property.Value)
        }
    }

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($pairs -join ';')
        $hash = $sha.ComputeHash($bytes)
        return (($hash | ForEach-Object { $_.ToString('x2') }) -join '')
    }
    finally {
        $sha.Dispose()
    }
}

function Select-MpcvrAutoTuneCandidate {
    param(
        [Parameter(Mandatory)]
        [object]$Recommendation,
        [string[]]$TriedSignatures = @()
    )

    $tried = @{}
    foreach ($signature in @($TriedSignatures)) {
        if (-not [string]::IsNullOrWhiteSpace($signature)) {
            $tried[$signature] = $true
        }
    }

    $ordered = @($Recommendation.Candidates | Sort-Object `
        @{ Expression = { if ($_.Recommended) { 0 } else { 1 } } }, `
        @{ Expression = { [int]$_.Rank } })
    foreach ($candidate in $ordered) {
        if ($candidate.Id -in @('keep-measured', 'no-automatic-change')) {
            continue
        }
        $signature = Get-MpcvrSettingsSignature -Settings $candidate.Settings
        if (-not $tried.ContainsKey($signature)) {
            return [pscustomobject]@{
                Candidate = $candidate
                Signature = $signature
            }
        }
    }
    return $null
}

function Start-MpcvrOwnedPlayer {
    param(
        [Parameter(Mandatory)]
        [string]$PlayerPath,
        [Parameter(Mandatory)]
        [string]$MediaPath
    )

    if (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
        throw 'Close every existing MPC-HC process before automatic calibration. The tuner only controls a player process that it launches itself.'
    }

    $quotedMedia = '"{0}"' -f $MediaPath.Replace('"', '\"')
    $process = Start-Process `
        -FilePath $PlayerPath `
        -ArgumentList $quotedMedia `
        -PassThru
    if ($null -eq $process) {
        throw 'MPC-HC did not return a process handle.'
    }
    return $process
}

function Stop-MpcvrOwnedPlayer {
    param([System.Diagnostics.Process]$Process)

    if ($null -eq $Process) {
        return
    }
    try {
        $Process.Refresh()
        if ($Process.HasExited) {
            return
        }
        [void]$Process.CloseMainWindow()
        if (-not $Process.WaitForExit(5000)) {
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
            [void]$Process.WaitForExit(5000)
        }
    }
    catch {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    }
}

function Test-MpcvrAutoTunePlanner {
    $candidateA = [pscustomobject]@{
        Id = 'first'
        Rank = 1
        Recommended = $true
        Settings = [pscustomobject]@{ MaxineOperation = 1; MaxineQuality = 2 }
    }
    $candidateB = [pscustomobject]@{
        Id = 'second'
        Rank = 2
        Recommended = $false
        Settings = [pscustomobject]@{ MaxineOperation = 0; MaxineQuality = 2 }
    }
    $recommendation = [pscustomobject]@{ Candidates = @($candidateA, $candidateB) }
    $first = Select-MpcvrAutoTuneCandidate -Recommendation $recommendation
    if ($null -eq $first -or $first.Candidate.Id -ne 'first') {
        return $false
    }
    $second = Select-MpcvrAutoTuneCandidate `
        -Recommendation $recommendation `
        -TriedSignatures @($first.Signature)
    return $null -ne $second -and $second.Candidate.Id -eq 'second'
}

Export-ModuleMember -Function @(
    'Get-MpcvrKnownPlayerPaths',
    'Resolve-MpcvrPlayerPath',
    'Get-MpcvrSettingsSignature',
    'Select-MpcvrAutoTuneCandidate',
    'Start-MpcvrOwnedPlayer',
    'Stop-MpcvrOwnedPlayer',
    'Test-MpcvrAutoTunePlanner'
)
