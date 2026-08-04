#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$SourcePath = $env:NV_VIDEO_EFFECTS_PATH,
    [string]$OutputDirectory = ([Environment]::GetFolderPath('Desktop')),
    [switch]$IncludeAllFiles,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$assetName = 'MPCVR-Maxine-Runtime.zip'
$checksumName = "$assetName.sha256"
$requiredFiles = @(
    'NVCVImage.dll',
    'NVVideoEffects.dll',
    'nvngxruntime.dll',
    'nvngx_vsr.dll',
    'nvVFXVideoSuperRes.dll'
)
$noticeNamePattern = '^(license|notice|eula|third[-_ ]?party|readme)(\.|-|_|$)'

function Complete-Run {
    param([int]$ExitCode)

    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

function Get-CompatibleRuntimeDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SearchRoot
    )

    $candidateRoots = @($SearchRoot)
    $candidateRoots += (Join-Path $SearchRoot 'nvvfx\libs')
    $candidateRoots += (Join-Path $SearchRoot '_internal\nvvfx\libs')

    foreach ($candidate in $candidateRoots | Select-Object -Unique) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Container)) {
            continue
        }
        $missing = @($requiredFiles | Where-Object {
            -not (Test-Path -LiteralPath (Join-Path $candidate $_) -PathType Leaf)
        })
        if ($missing.Count -eq 0) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw ('No compatible VideoSuperRes runtime directory was found. Required files: {0}' -f ($requiredFiles -join ', '))
}

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'This exporter only supports Windows.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if ([string]::IsNullOrWhiteSpace($SourcePath)) {
    Write-Host 'NV_VIDEO_EFFECTS_PATH is not set. Pass -SourcePath with the working runtime directory.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("MPCVR-Maxine-Export-{0}" -f [guid]::NewGuid())
$packageRoot = Join-Path $tempRoot 'MPCVR-Maxine-Runtime'
$packageRuntime = Join-Path $packageRoot 'nvvfx\libs'
$archivePath = Join-Path $OutputDirectory $assetName
$checksumPath = Join-Path $OutputDirectory $checksumName
$exitCode = 0

try {
    $runtimeDirectory = Get-CompatibleRuntimeDirectory -SearchRoot $SourcePath
    New-Item -ItemType Directory -Path $packageRuntime -Force | Out-Null
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

    $allSourceFiles = @(Get-ChildItem -LiteralPath $runtimeDirectory -File -Recurse)
    if ($IncludeAllFiles) {
        Get-ChildItem -LiteralPath $runtimeDirectory -Force | Copy-Item -Destination $packageRuntime -Recurse -Force
        $profile = 'FullDirectory'
    }
    else {
        foreach ($fileName in $requiredFiles) {
            Copy-Item -LiteralPath (Join-Path $runtimeDirectory $fileName) -Destination $packageRuntime -Force
        }

        # Carry obvious license, notice, and README files when they are present,
        # but intentionally exclude the large optional TensorRT and NPP DLLs.
        Get-ChildItem -LiteralPath $runtimeDirectory -File | Where-Object {
            $_.Name -match $noticeNamePattern
        } | Copy-Item -Destination $packageRuntime -Force
        $profile = 'CoreFiveFiles'
    }

    $packagedFiles = @(Get-ChildItem -LiteralPath $packageRuntime -File -Recurse)
    $files = @($packagedFiles | ForEach-Object {
        [pscustomobject]@{
            Path = $_.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
            Size = $_.Length
            SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })

    $packagedNames = @($packagedFiles | ForEach-Object { $_.FullName.Substring($packageRuntime.Length + 1).Replace('\', '/') })
    $excludedFiles = @($allSourceFiles | Where-Object {
        $_.FullName.Substring($runtimeDirectory.Length + 1).Replace('\', '/') -notin $packagedNames
    } | Sort-Object Length -Descending | ForEach-Object {
        [pscustomobject]@{
            Path = $_.FullName.Substring($runtimeDirectory.Length + 1).Replace('\', '/')
            Size = $_.Length
        }
    })

    $manifest = [ordered]@{
        Format = 2
        CreatedUtc = [DateTime]::UtcNow.ToString('o')
        Profile = $profile
        RequiredFiles = $requiredFiles
        PackagedBytes = [long](($packagedFiles | Measure-Object -Property Length -Sum).Sum)
        ExcludedBytes = [long](($excludedFiles | Measure-Object -Property Size -Sum).Sum)
        Files = $files
        ExcludedFiles = $excludedFiles
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $packageRoot 'runtime-manifest.json') -Encoding UTF8

    Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $checksumPath -Force -ErrorAction SilentlyContinue
    Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -CompressionLevel Optimal

    $hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $assetName" | Set-Content -LiteralPath $checksumPath -Encoding ASCII

    $archiveItem = Get-Item -LiteralPath $archivePath
    $sourceBytes = [long](($allSourceFiles | Measure-Object -Property Length -Sum).Sum)
    $packagedBytes = [long](($packagedFiles | Measure-Object -Property Length -Sum).Sum)

    Write-Host 'Created the Maxine runtime release assets:' -ForegroundColor Green
    Write-Host $archivePath
    Write-Host $checksumPath
    Write-Host
    Write-Host ('Profile: {0}' -f $profile)
    Write-Host ('Source directory: {0:N1} MiB' -f ($sourceBytes / 1MB))
    Write-Host ('Packaged files: {0:N1} MiB' -f ($packagedBytes / 1MB))
    Write-Host ('Compressed archive: {0:N1} MiB' -f ($archiveItem.Length / 1MB))
    if (-not $IncludeAllFiles) {
        Write-Host ('Excluded optional files: {0:N1} MiB' -f (($sourceBytes - $packagedBytes) / 1MB))
        Write-Host 'This core candidate must be runtime-tested before public release.' -ForegroundColor Yellow
    }
    Write-Host
    Write-Host 'Review NVIDIA licensing and include any required license or notice files before publishing these assets.' -ForegroundColor Yellow
}
catch {
    $exitCode = 1
    Write-Host
    Write-Host "Runtime export failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Complete-Run -ExitCode $exitCode
