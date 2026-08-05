#requires -Version 5.1

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:OfficialOpticalFlowSdkUrl = 'https://developer.nvidia.com/downloads/designworks/optical-flow-sdk/secure/5.0/optical_flow_sdk_5.0.7.zip/'
$script:ExpectedRuntimeHashes = [ordered]@{
    'NvOFFRUC.dll' = '5a0b6701d30709e25e7e5b92ca46b18aab1459160cecd4f629872369d85c8b0a'
    'cudart64_110.dll' = 'edc35e7d0fa3f257bbedfa7888911080c5696acdd40b6187b6dd0173f20759ad'
}

function Get-MpcvrOpticalFlowOfficialDownloadUrl {
    return $script:OfficialOpticalFlowSdkUrl
}

function Get-MpcvrDownloadsDirectory {
    $knownFolderValue = '{374DE290-123F-4565-9164-39C4925E467B}'
    try {
        $shellFolders = Get-ItemProperty `
            -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders' `
            -ErrorAction Stop
        $configured = [string]$shellFolders.$knownFolderValue
        if (-not [string]::IsNullOrWhiteSpace($configured)) {
            $expanded = [Environment]::ExpandEnvironmentVariables($configured)
            if (Test-Path -LiteralPath $expanded -PathType Container) {
                return [IO.Path]::GetFullPath($expanded)
            }
        }
    }
    catch {
        # Fall through to the conventional profile location.
    }

    $fallback = Join-Path $env:USERPROFILE 'Downloads'
    if (-not (Test-Path -LiteralPath $fallback -PathType Container)) {
        New-Item -ItemType Directory -Path $fallback -Force | Out-Null
    }
    return [IO.Path]::GetFullPath($fallback)
}

function Test-MpcvrOpticalFlowSdkArchive {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    if ([IO.Path]::GetExtension($Path) -ine '.zip') {
        return $false
    }

    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($Path))
        try {
            $entryNames = @($archive.Entries | ForEach-Object {
                (([string]$_.FullName) -replace '\\', '/').TrimStart('/')
            })
            $requiredSuffixes = @(
                'NvOFFRUC/NvOFFRUCSample/bin/win64/NvOFFRUC.dll',
                'NvOFFRUC/NvOFFRUCSample/bin/win64/cudart64_110.dll'
            )
            foreach ($suffix in $requiredSuffixes) {
                if (@($entryNames | Where-Object { $_ -like "*$suffix" }).Count -eq 0) {
                    return $false
                }
            }
            return $true
        }
        finally {
            $archive.Dispose()
        }
    }
    catch {
        return $false
    }
}

function Find-MpcvrOpticalFlowSdkArchive {
    [CmdletBinding()]
    param(
        [string[]]$SearchDirectories,
        [datetime]$NotOlderThan = [datetime]::MinValue
    )

    if ($null -eq $SearchDirectories -or $SearchDirectories.Count -eq 0) {
        $SearchDirectories = @(Get-MpcvrDownloadsDirectory)
    }

    $candidates = @()
    foreach ($directory in @($SearchDirectories | Select-Object -Unique)) {
        if ([string]::IsNullOrWhiteSpace($directory) -or
            -not (Test-Path -LiteralPath $directory -PathType Container)) {
            continue
        }
        $candidates += @(Get-ChildItem -LiteralPath $directory -File -Filter '*.zip' -ErrorAction SilentlyContinue |
            Where-Object {
                $_.LastWriteTimeUtc -ge $NotOlderThan.ToUniversalTime() -and
                $_.Name -match '(?i)(optical[ _-]*flow|nvof)'
            })
    }

    foreach ($candidate in @($candidates | Sort-Object LastWriteTimeUtc -Descending)) {
        if (Test-MpcvrOpticalFlowSdkArchive -Path $candidate.FullName) {
            return $candidate.FullName
        }
    }
    return $null
}

function Select-MpcvrOpticalFlowSdkArchive {
    [CmdletBinding()]
    param([string]$InitialDirectory)

    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select the NVIDIA Optical Flow SDK ZIP'
    $dialog.Filter = 'NVIDIA Optical Flow SDK ZIP (*.zip)|*.zip|All files (*.*)|*.*'
    $dialog.CheckFileExists = $true
    if (-not [string]::IsNullOrWhiteSpace($InitialDirectory) -and
        (Test-Path -LiteralPath $InitialDirectory -PathType Container)) {
        $dialog.InitialDirectory = $InitialDirectory
    }
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        throw 'No NVIDIA Optical Flow SDK package was selected.'
    }
    if (-not (Test-MpcvrOpticalFlowSdkArchive -Path $dialog.FileName)) {
        throw 'The selected ZIP is not a compatible NVIDIA Optical Flow SDK package.'
    }
    return $dialog.FileName
}

function Wait-MpcvrOpticalFlowSdkArchive {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$DownloadsDirectory,
        [Parameter(Mandatory)]
        [datetime]$StartedAtUtc,
        [ValidateRange(1, 60)]
        [int]$WaitMinutes = 15
    )

    $deadline = [DateTime]::UtcNow.AddMinutes($WaitMinutes)
    $lastStatus = [DateTime]::MinValue
    while ([DateTime]::UtcNow -lt $deadline) {
        $candidate = Find-MpcvrOpticalFlowSdkArchive `
            -SearchDirectories @($DownloadsDirectory) `
            -NotOlderThan $StartedAtUtc.AddSeconds(-5)
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            return $candidate
        }

        if (([DateTime]::UtcNow - $lastStatus).TotalSeconds -ge 15) {
            $remaining = [Math]::Max(0, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalMinutes))
            Write-Host "Waiting for the NVIDIA SDK download in $DownloadsDirectory ($remaining minute(s) remaining)..." -ForegroundColor Cyan
            $lastStatus = [DateTime]::UtcNow
        }
        Start-Sleep -Seconds 1
    }
    return $null
}

function Copy-MpcvrOpticalFlowSdkToCache {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$SourcePath,
        [string]$CacheRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup\downloads')
    )

    $CacheRoot = [IO.Path]::GetFullPath($CacheRoot)
    New-Item -ItemType Directory -Path $CacheRoot -Force | Out-Null
    $hash = (Get-FileHash -LiteralPath $SourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $destination = Join-Path $CacheRoot ("Optical_Flow_SDK_5.0.7-$hash.zip")
    if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        Copy-Item -LiteralPath $SourcePath -Destination $destination -Force
    }
    if (-not (Test-MpcvrOpticalFlowSdkArchive -Path $destination)) {
        Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
        throw 'The cached NVIDIA Optical Flow SDK failed structure validation.'
    }
    return $destination
}

function Resolve-MpcvrOpticalFlowSdkPackage {
    [CmdletBinding()]
    param(
        [string]$ExplicitPath,
        [string]$PayloadDirectory,
        [string]$CacheRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup\downloads'),
        [ValidateRange(1, 60)]
        [int]$WaitMinutes = 15,
        [switch]$DisableOfficialDownload
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $resolved = (Resolve-Path -LiteralPath $ExplicitPath).Path
        if (Test-Path -LiteralPath $resolved -PathType Container) {
            return $resolved
        }
        if (-not (Test-MpcvrOpticalFlowSdkArchive -Path $resolved)) {
            throw 'The supplied SDK ZIP is not a compatible NVIDIA Optical Flow SDK package.'
        }
        return $resolved
    }

    $downloads = Get-MpcvrDownloadsDirectory
    $searchDirectories = @($CacheRoot, $downloads)
    if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory)) {
        $searchDirectories = @($PayloadDirectory) + $searchDirectories
    }
    $existing = Find-MpcvrOpticalFlowSdkArchive -SearchDirectories $searchDirectories
    if (-not [string]::IsNullOrWhiteSpace($existing)) {
        Write-Host "Using an existing NVIDIA Optical Flow SDK package: $existing" -ForegroundColor Cyan
        return $existing
    }

    if ($DisableOfficialDownload) {
        return Select-MpcvrOpticalFlowSdkArchive -InitialDirectory $downloads
    }

    Write-Host
    Write-Host 'NVIDIA requires a free Developer Program login and license acceptance before downloading the Optical Flow SDK.' -ForegroundColor Yellow
    Write-Host 'Your browser will open the official NVIDIA page. Sign in and choose Accept & Download.' -ForegroundColor Yellow
    Write-Host 'Setup will detect the completed ZIP automatically; you do not need to locate it afterward.' -ForegroundColor Yellow
    Write-Host

    $startedAt = [DateTime]::UtcNow
    Start-Process -FilePath $script:OfficialOpticalFlowSdkUrl
    $downloaded = Wait-MpcvrOpticalFlowSdkArchive `
        -DownloadsDirectory $downloads `
        -StartedAtUtc $startedAt `
        -WaitMinutes $WaitMinutes

    if ([string]::IsNullOrWhiteSpace($downloaded)) {
        Write-Warning 'The SDK was not detected in the Downloads folder before the wait period expired.'
        $downloaded = Select-MpcvrOpticalFlowSdkArchive -InitialDirectory $downloads
    }

    $cached = Copy-MpcvrOpticalFlowSdkToCache -SourcePath $downloaded -CacheRoot $CacheRoot
    Write-Host "Validated and cached the NVIDIA SDK at: $cached" -ForegroundColor Green
    return $cached
}

function Test-MpcvrNvOffrucRuntimeHashes {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$NvOffrucPath,
        [Parameter(Mandatory)]
        [string]$CudaRuntimePath,
        [switch]$AllowUnverifiedRuntimeFiles
    )

    $paths = [ordered]@{
        'NvOFFRUC.dll' = $NvOffrucPath
        'cudart64_110.dll' = $CudaRuntimePath
    }
    foreach ($name in $paths.Keys) {
        $actual = (Get-FileHash -LiteralPath $paths[$name] -Algorithm SHA256).Hash.ToLowerInvariant()
        $expected = [string]$script:ExpectedRuntimeHashes[$name]
        if ($actual -ne $expected) {
            $message = "$name does not match the verified Optical Flow SDK 5.0.7 runtime hash. Expected $expected; got $actual."
            if ($AllowUnverifiedRuntimeFiles) {
                Write-Warning "$message Continuing because AllowUnverifiedRuntimeFiles was explicitly supplied."
            }
            else {
                throw $message
            }
        }
    }
    return $true
}

Export-ModuleMember -Function @(
    'Get-MpcvrOpticalFlowOfficialDownloadUrl',
    'Get-MpcvrDownloadsDirectory',
    'Test-MpcvrOpticalFlowSdkArchive',
    'Find-MpcvrOpticalFlowSdkArchive',
    'Select-MpcvrOpticalFlowSdkArchive',
    'Wait-MpcvrOpticalFlowSdkArchive',
    'Copy-MpcvrOpticalFlowSdkToCache',
    'Resolve-MpcvrOpticalFlowSdkPackage',
    'Test-MpcvrNvOffrucRuntimeHashes'
)


