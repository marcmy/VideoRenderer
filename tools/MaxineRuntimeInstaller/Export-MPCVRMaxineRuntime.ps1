#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$SourcePath = $env:NV_VIDEO_EFFECTS_PATH,
    [string]$OutputDirectory = ([Environment]::GetFolderPath('Desktop')),
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

    Get-ChildItem -LiteralPath $runtimeDirectory -Force | Copy-Item -Destination $packageRuntime -Recurse -Force

    $files = Get-ChildItem -LiteralPath $packageRuntime -File -Recurse | ForEach-Object {
        [pscustomobject]@{
            Path = $_.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
            Size = $_.Length
            SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }

    $manifest = [ordered]@{
        Format = 1
        CreatedUtc = [DateTime]::UtcNow.ToString('o')
        RequiredFiles = $requiredFiles
        Files = @($files)
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $packageRoot 'runtime-manifest.json') -Encoding UTF8

    Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $checksumPath -Force -ErrorAction SilentlyContinue
    Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -CompressionLevel Optimal

    $hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $assetName" | Set-Content -LiteralPath $checksumPath -Encoding ASCII

    Write-Host 'Created the Maxine runtime release assets:' -ForegroundColor Green
    Write-Host $archivePath
    Write-Host $checksumPath
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
