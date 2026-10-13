#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$PackageArchive,
    [string]$ChecksumFile,
    [switch]$NoPause,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

Import-Module (Join-Path $PSScriptRoot 'KLiteRendererInstall.psm1') -Force

$releaseBaseUrl = 'https://github.com/marcmy/VideoRenderer/releases/latest/download'
$setupAssetName = 'MPCVR-Setup.zip'
$primaryAssetName = 'MPCVR-Portable.zip'
$previousAssetName = 'MpcVideoRenderer-Maxine-RIFE.zip'
$legacyAssetName = 'MpcVideoRenderer-Maxine.zip'
$checksumListName = 'SHA256SUMS.txt'

$targets = [ordered]@{
    'MpcVideoRenderer.ax' = 'C:\Program Files (x86)\K-Lite Codec Pack\Filters\MPCVR\MpcVideoRenderer.ax'
    'MpcVideoRenderer64.ax' = 'C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax'
}

function Test-IsWindowsPlatform {
    $isWindowsVariable = Get-Variable -Name IsWindows -ErrorAction SilentlyContinue
    if ($null -ne $isWindowsVariable) {
        return [bool]$isWindowsVariable.Value
    }

    return $env:OS -eq 'Windows_NT'
}

function Get-PowerShellExecutable {
    $pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
    if ($null -ne $pwsh) {
        return $pwsh.Source
    }

    $windowsPowerShell = Get-Command powershell.exe -ErrorAction SilentlyContinue
    if ($null -ne $windowsPowerShell) {
        return $windowsPowerShell.Source
    }

    throw 'Neither PowerShell 7 (pwsh.exe) nor Windows PowerShell (powershell.exe) was found.'
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object -TypeName Security.Principal.WindowsPrincipal -ArgumentList $identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-TargetDirectory {
    param(
        [Parameter(Mandatory)]
        [string]$TargetPath
    )

    $directory = [IO.Path]::GetDirectoryName($TargetPath)
    if ([string]::IsNullOrWhiteSpace($directory)) {
        throw "Could not determine the parent directory for: $TargetPath"
    }
    return $directory
}

function Quote-ProcessArgument {
    param(
        [Parameter(Mandatory)]
        [string]$Value
    )

    return '"{0}"' -f $Value.Replace('"', '\"')
}

function Get-ExpectedHash {
    param(
        [Parameter(Mandatory)]
        [string]$ChecksumPath,
        [Parameter(Mandatory)]
        [string]$FileName
    )

    $text = Get-Content -LiteralPath $ChecksumPath -Raw
    foreach ($line in ($text -split "`r?`n")) {
        $match = [regex]::Match($line, '^\s*([a-fA-F0-9]{64})\s+\*?(.+?)\s*$')
        if ($match.Success -and $match.Groups[2].Value -ieq $FileName) {
            return $match.Groups[1].Value.ToLowerInvariant()
        }
    }

    $singleHash = [regex]::Match($text, '(?i)\b[a-f0-9]{64}\b')
    if ($singleHash.Success) {
        return $singleHash.Value.ToLowerInvariant()
    }

    throw "No valid SHA-256 entry for $FileName was found in $ChecksumPath."
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

if (-not (Test-IsWindowsPlatform)) {
    Write-Host 'This updater only supports Windows.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if ([string]::IsNullOrWhiteSpace($PackageArchive) -xor [string]::IsNullOrWhiteSpace($ChecksumFile)) {
    Write-Host 'PackageArchive and ChecksumFile must be supplied together.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if ($ValidateOnly) {
    foreach ($destination in $targets.Values) {
        $directory = Get-TargetDirectory -TargetPath $destination
        if (-not [IO.Path]::IsPathRooted($directory)) {
            throw "Target directory is not fully qualified: $directory"
        }
    }

    [void](Get-PowerShellExecutable)
    [void](Get-Command Install-KLiteRendererFiles -CommandType Function -ErrorAction Stop)
    if (-not [string]::IsNullOrWhiteSpace($PackageArchive)) {
        $resolvedArchive = (Resolve-Path -LiteralPath $PackageArchive).Path
        $resolvedChecksum = (Resolve-Path -LiteralPath $ChecksumFile).Path
        [void](Get-ExpectedHash -ChecksumPath $resolvedChecksum -FileName ([IO.Path]::GetFileName($resolvedArchive)))
    }

    Write-Host "K-Lite updater validation passed under PowerShell $($PSVersionTable.PSVersion)." -ForegroundColor Green
    Complete-Run -ExitCode 0
}

if (-not (Test-IsAdministrator)) {
    $powerShellExecutable = Get-PowerShellExecutable
    $argumentList = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File {0}' -f (Quote-ProcessArgument -Value $PSCommandPath)

    if (-not [string]::IsNullOrWhiteSpace($PackageArchive)) {
        $resolvedArchive = (Resolve-Path -LiteralPath $PackageArchive).Path
        $resolvedChecksum = (Resolve-Path -LiteralPath $ChecksumFile).Path
        $argumentList += ' -PackageArchive {0} -ChecksumFile {1}' -f `
            (Quote-ProcessArgument -Value $resolvedArchive), `
            (Quote-ProcessArgument -Value $resolvedChecksum)
    }
    if ($NoPause) {
        $argumentList += ' -NoPause'
    }

    try {
        $process = Start-Process -FilePath $powerShellExecutable -ArgumentList $argumentList -Verb RunAs -Wait -PassThru
        exit $process.ExitCode
    }
    catch {
        Write-Host "Administrator elevation was cancelled or failed: $($_.Exception.Message)" -ForegroundColor Red
        Complete-Run -ExitCode 1
    }
}

if ($PSVersionTable.PSEdition -eq 'Desktop') {
    [Net.ServicePointManager]::SecurityProtocol =
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("MPCVR-Maxine-Updater-{0}" -f [guid]::NewGuid())
$checksumPath = Join-Path $tempRoot $checksumListName
$extractPath = Join-Path $tempRoot 'extracted'
$archivePath = $null
$selectedAssetName = $primaryAssetName
$exitCode = 0

try {
    if (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
        Write-Host 'MPC-HC is open. Close it to continue installation automatically (Ctrl+C to cancel).' -ForegroundColor Yellow
        while (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
            Start-Sleep -Milliseconds 500
        }
        Write-Host 'MPC-HC has closed. Continuing installation...' -ForegroundColor Green
    }

    foreach ($destination in $targets.Values) {
        $destinationDirectory = Get-TargetDirectory -TargetPath $destination
        if (-not (Test-Path -LiteralPath $destinationDirectory -PathType Container)) {
            throw "K-Lite destination directory was not found: $destinationDirectory"
        }
    }

    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

    if (-not [string]::IsNullOrWhiteSpace($PackageArchive)) {
        $resolvedArchive = (Resolve-Path -LiteralPath $PackageArchive).Path
        $resolvedChecksum = (Resolve-Path -LiteralPath $ChecksumFile).Path
        $selectedAssetName = [IO.Path]::GetFileName($resolvedArchive)
        $archivePath = Join-Path $tempRoot $selectedAssetName
        Copy-Item -LiteralPath $resolvedArchive -Destination $archivePath -Force
        Copy-Item -LiteralPath $resolvedChecksum -Destination $checksumPath -Force
        Write-Host 'Using the renderer package included with MPCVR Maxine + RIFE Setup...'
    }
    else {
        Write-Host 'Downloading the latest custom Maxine + RIFE build...'
        # Resolve latest once. Archive and sums must come from the same release,
        # even if a new release is promoted while this updater is downloading.
        $releaseRequest=@{Uri='https://api.github.com/repos/marcmy/VideoRenderer/releases/latest';UseBasicParsing=$true}
        $release=([string](Invoke-WebRequest @releaseRequest).Content) | ConvertFrom-Json
        if ([string]::IsNullOrWhiteSpace([string]$release.tag_name)) { throw 'Latest renderer release has no source tag.' }
        $releaseBaseUrl='https://github.com/marcmy/VideoRenderer/releases/download/'+[Uri]::EscapeDataString([string]$release.tag_name)
        $downloaded=$false
        foreach ($candidate in @($setupAssetName,$primaryAssetName,$previousAssetName,$legacyAssetName)) {
          try {
            $selectedAssetName = $candidate
            $archivePath = Join-Path $tempRoot $selectedAssetName
            $archiveRequest = @{
                Uri = "$releaseBaseUrl/$selectedAssetName"
                OutFile = $archivePath
            }
            if ($PSVersionTable.PSEdition -eq 'Desktop') {
                $archiveRequest.UseBasicParsing = $true
            }
            Invoke-WebRequest @archiveRequest
            $downloaded=$true
            break
          } catch {
            Write-Host "Renderer asset $candidate was unavailable; checking the next supported filename." -ForegroundColor Yellow
          }
        }
        if (-not $downloaded) { throw 'No supported renderer asset could be downloaded.' }

        try {
            $checksumRequest = @{
                Uri = "$releaseBaseUrl/$checksumListName"
                OutFile = $checksumPath
            }
            if ($PSVersionTable.PSEdition -eq 'Desktop') {
                $checksumRequest.UseBasicParsing = $true
            }
            Invoke-WebRequest @checksumRequest
        }
        catch {
            Write-Host 'The combined checksum list was unavailable; trying the legacy checksum asset.' -ForegroundColor Yellow
            $legacyChecksumName = "$selectedAssetName.sha256"
            $legacyRequest = @{
                Uri = "$releaseBaseUrl/$legacyChecksumName"
                OutFile = $checksumPath
            }
            if ($PSVersionTable.PSEdition -eq 'Desktop') {
                $legacyRequest.UseBasicParsing = $true
            }
            Invoke-WebRequest @legacyRequest
        }
    }

    $expectedHash = Get-ExpectedHash -ChecksumPath $checksumPath -FileName $selectedAssetName
    $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "SHA-256 verification failed. Expected $expectedHash but found $actualHash."
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath -Force

    if ($selectedAssetName -eq $setupAssetName) {
        # A restore/update also reconciles independently versioned dependencies.
        # Setup calls us with its local renderer archive, so delegation cannot recurse.
        $setupScript=Join-Path $extractPath 'Install-MPCVR-Maxine-RIFE.ps1'
        if (-not (Test-Path -LiteralPath $setupScript -PathType Leaf)) { throw 'Verified setup is missing its installer.' }
        $hostExecutable=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $previousModulePath=$env:PSModulePath
        try {
            $env:PSModulePath=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules'
            & $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File $setupScript -NoPause
            if ($LASTEXITCODE -ne 0) { throw 'The verified setup could not complete the update; component rollback diagnostics are shown above.' }
        } finally { $env:PSModulePath=$previousModulePath }
        Complete-Run -ExitCode 0
    }

    $sources = @{}
    foreach ($fileName in $targets.Keys) {
        $source = Join-Path $extractPath $fileName
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "The renderer package does not contain $fileName."
        }
        $sources[$fileName] = $source
    }

    $installTargets = @{}
    foreach ($fileName in $targets.Keys) {
        $installTargets[$fileName] = $targets[$fileName]
        Write-Host "Installing $fileName..."
    }
    $installedFiles = @(Install-KLiteRendererFiles -Sources $sources -Targets $installTargets)

    Write-Host
    Write-Host 'Custom MPC Video Renderer Maxine + RIFE build restored successfully.' -ForegroundColor Green
    $installedFiles | Select-Object File, Version, Path | Format-Table -AutoSize
}
catch {
    $exitCode = 1
    Write-Host
    Write-Host "Update failed: $($_.Exception.Message)" -ForegroundColor Red

    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Complete-Run -ExitCode $exitCode
