#requires -Version 7.0

[CmdletBinding()]
param(
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$releaseBaseUrl = 'https://github.com/marcmy/VideoRenderer/releases/download/maxine-latest'
$assetName = 'MpcVideoRenderer-Maxine-latest.zip'
$checksumName = "$assetName.sha256"

$targets = [ordered]@{
    'MpcVideoRenderer.ax' = 'C:\Program Files (x86)\K-Lite Codec Pack\Filters\MPCVR\MpcVideoRenderer.ax'
    'MpcVideoRenderer64.ax' = 'C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax'
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Complete-Run {
    param(
        [int]$ExitCode
    )

    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

if (-not $IsWindows) {
    Write-Host 'This updater only supports Windows.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if (-not (Test-IsAdministrator)) {
    $pwsh = (Get-Command pwsh.exe -ErrorAction Stop).Source
    $arguments = @(
        '-NoLogo'
        '-NoProfile'
        '-ExecutionPolicy'
        'Bypass'
        '-File'
        ('"{0}"' -f $PSCommandPath)
    )
    if ($NoPause) {
        $arguments += '-NoPause'
    }

    try {
        $process = Start-Process -FilePath $pwsh -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        exit $process.ExitCode
    }
    catch {
        Write-Host "Administrator elevation was cancelled or failed: $($_.Exception.Message)" -ForegroundColor Red
        Complete-Run -ExitCode 1
    }
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("MPCVR-Maxine-Updater-{0}" -f [guid]::NewGuid())
$archivePath = Join-Path $tempRoot $assetName
$checksumPath = Join-Path $tempRoot $checksumName
$extractPath = Join-Path $tempRoot 'extracted'
$exitCode = 0

try {
    $runningPlayers = Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue
    if ($runningPlayers) {
        throw 'Close MPC-HC before updating MPC Video Renderer.'
    }

    foreach ($destination in $targets.Values) {
        $destinationDirectory = Split-Path -LiteralPath $destination -Parent
        if (-not (Test-Path -LiteralPath $destinationDirectory -PathType Container)) {
            throw "K-Lite destination directory was not found: $destinationDirectory"
        }
    }

    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

    Write-Host 'Downloading the latest custom Maxine build...'
    Invoke-WebRequest -Uri "$releaseBaseUrl/$assetName" -OutFile $archivePath
    Invoke-WebRequest -Uri "$releaseBaseUrl/$checksumName" -OutFile $checksumPath

    $expectedHash = ((Get-Content -LiteralPath $checksumPath -Raw) -split '\s+')[0].Trim().ToLowerInvariant()
    if ($expectedHash -notmatch '^[a-f0-9]{64}$') {
        throw 'The published SHA-256 file is malformed.'
    }

    $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "SHA-256 verification failed. Expected $expectedHash but downloaded $actualHash."
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath -Force

    foreach ($fileName in $targets.Keys) {
        $source = Join-Path $extractPath $fileName
        $destination = $targets[$fileName]

        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "The downloaded package does not contain $fileName."
        }

        Write-Host "Installing $fileName..."
        Copy-Item -LiteralPath $source -Destination $destination -Force

        $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        $destinationHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
        if ($sourceHash -ne $destinationHash) {
            throw "Verification failed after copying $fileName."
        }
    }

    Write-Host
    Write-Host 'Custom MPC Video Renderer restored successfully.' -ForegroundColor Green
    $targets.Values | ForEach-Object {
        $item = Get-Item -LiteralPath $_
        [pscustomobject]@{
            File = $item.Name
            Version = $item.VersionInfo.FileVersion
            Path = $item.FullName
        }
    } | Format-Table -AutoSize
}
catch {
    $exitCode = 1
    Write-Host
    Write-Host "Update failed: $($_.Exception.Message)" -ForegroundColor Red
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Complete-Run -ExitCode $exitCode
