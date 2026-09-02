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

$releaseBaseUrl = 'https://github.com/marcmy/VideoRenderer/releases/latest/download'
$assetName = 'MpcVideoRenderer-Maxine.zip'
$checksumListName = 'SHA256SUMS.txt'
$legacyChecksumName = "$assetName.sha256"

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

function Get-AvailableTargets {
    $available = [ordered]@{}
    foreach ($fileName in $targets.Keys) {
        $destination = $targets[$fileName]
        $directory = Get-TargetDirectory -TargetPath $destination
        if (Test-Path -LiteralPath $directory -PathType Container) {
            $available[$fileName] = $destination
        }
    }
    return $available
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

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("MPCVR-Unified-Updater-{0}" -f [guid]::NewGuid())
$archivePath = Join-Path $tempRoot $assetName
$checksumPath = Join-Path $tempRoot $checksumListName
$extractPath = Join-Path $tempRoot 'extracted'
$exitCode = 0

try {
    $runningPlayers = Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue
    if ($runningPlayers) {
        throw 'Close MPC-HC before updating MPC Video Renderer.'
    }

    $availableTargets = Get-AvailableTargets
    if ($availableTargets.Count -eq 0) {
        $expectedDirectories = @($targets.Values | ForEach-Object { Get-TargetDirectory -TargetPath $_ })
        throw ('No supported K-Lite MPC-HC renderer directory was found. Checked: {0}' -f ($expectedDirectories -join '; '))
    }

    $skippedTargets = @($targets.Keys | Where-Object { -not $availableTargets.Contains($_) })
    if ($skippedTargets.Count -gt 0) {
        Write-Host ('Skipping absent K-Lite targets: {0}' -f ($skippedTargets -join ', ')) -ForegroundColor DarkGray
    }

    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

    if (-not [string]::IsNullOrWhiteSpace($PackageArchive)) {
        $resolvedArchive = (Resolve-Path -LiteralPath $PackageArchive).Path
        $resolvedChecksum = (Resolve-Path -LiteralPath $ChecksumFile).Path
        Copy-Item -LiteralPath $resolvedArchive -Destination $archivePath -Force
        Copy-Item -LiteralPath $resolvedChecksum -Destination $checksumPath -Force
        Write-Host 'Using the renderer package included with MPCVR Unified Setup...'
    }
    else {
        Write-Host 'Downloading the latest custom MPC Video Renderer build...'
        $archiveRequest = @{
            Uri = "$releaseBaseUrl/$assetName"
            OutFile = $archivePath
        }
        if ($PSVersionTable.PSEdition -eq 'Desktop') {
            $archiveRequest.UseBasicParsing = $true
        }
        Invoke-WebRequest @archiveRequest

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

    $expectedHash = Get-ExpectedHash -ChecksumPath $checksumPath -FileName $assetName
    $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "SHA-256 verification failed. Expected $expectedHash but found $actualHash."
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath -Force

    foreach ($fileName in $availableTargets.Keys) {
        $source = Join-Path $extractPath $fileName
        $destination = $availableTargets[$fileName]

        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "The renderer package does not contain $fileName."
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
    Write-Host 'Custom MPC Video Renderer installed successfully.' -ForegroundColor Green
    $availableTargets.Values | ForEach-Object {
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

    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Complete-Run -ExitCode $exitCode
