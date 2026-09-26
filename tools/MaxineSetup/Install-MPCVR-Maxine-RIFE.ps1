#requires -Version 5.1

[CmdletBinding()]
param(
    [switch]$NoPause,
    [switch]$ValidateOnly,
    [string]$GpuInventoryJson
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$powerShellExecutable = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$payloadRoot = Join-Path $PSScriptRoot 'payload'
$payloadChecksums = Join-Path $payloadRoot 'PAYLOAD-SHA256SUMS.txt'

$maxineRuntimeInstaller = Join-Path $payloadRoot 'Install-MPCVRMaxineRuntime.ps1'
$rifeRuntimeInstaller = Join-Path $payloadRoot 'Install-MPCVRRifeRuntime.ps1'
$rifePreflight = Join-Path $payloadRoot 'Test-MPCVRRifePreflight.ps1'
$updaterInstaller = Join-Path $payloadRoot 'Install-KLiteMPCVRUpdater.ps1'
$rendererUpdater = Join-Path $payloadRoot 'Update-KLiteMPCVR.ps1'

$rendererArchive = Join-Path $payloadRoot 'MpcVideoRenderer-Maxine-RIFE.zip'
$maxineRuntimeArchive = Join-Path $payloadRoot 'MPCVR-Maxine-Runtime.zip'
$rifeModelArchive = Join-Path $payloadRoot 'MPCVR-RIFE-Models.zip'
$rifeRuntimeManifest = Join-Path $payloadRoot 'RIFE-runtime-manifest.json'
$rifeRuntimeChecksums = Join-Path $payloadRoot 'RIFE-runtime-SHA256SUMS.txt'
$rifeRuntimeSource = Join-Path $payloadRoot 'RIFE-runtime-release.json'
$rifeManifestModule = Join-Path $payloadRoot 'RifeRuntimeManifest.psm1'

$requiredPayloadNames = @(
    'Install-MPCVRMaxineRuntime.ps1',
    'Install-MPCVRRifeRuntime.ps1',
    'Test-MPCVRRifePreflight.ps1',
    'RifeRuntimeManifest.psm1',
    'Install-KLiteMPCVRUpdater.ps1',
    'Update-KLiteMPCVR.ps1',
    'KLiteRendererInstall.psm1',
    'MpcVideoRenderer-Maxine-RIFE.zip',
    'MPCVR-Maxine-Runtime.zip',
    'MPCVR-RIFE-Common.zip',
    'MPCVR-RIFE-Models.zip',
    'RIFE-runtime-manifest.json',
    'RIFE-runtime-SHA256SUMS.txt',
    'RIFE-runtime-release.json',
    'PAYLOAD-SHA256SUMS.txt'
)

function Complete-Run {
    param([int]$ExitCode)

    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

function Get-ExpectedHash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ChecksumPath,
        [Parameter(Mandatory = $true)]
        [string]$FileName
    )

    $text = Get-Content -LiteralPath $ChecksumPath -Raw
    $hashes = @()
    foreach ($line in ($text -split "`r?`n")) {
        $match = [regex]::Match($line, '^\s*([a-fA-F0-9]{64})\s+\*?(.+?)\s*$')
        if (-not $match.Success) {
            continue
        }

        $hash = $match.Groups[1].Value.ToLowerInvariant()
        $name = $match.Groups[2].Value.Replace('\', '/')
        $hashes += $hash
        if ($name -ieq $FileName.Replace('\', '/')) {
            return $hash
        }
    }

    if ($hashes.Count -eq 1) {
        return $hashes[0]
    }

    throw "No valid SHA-256 entry for $FileName was found in $ChecksumPath."
}

function Test-FileHash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string]$ChecksumPath,
        [Parameter(Mandatory = $true)]
        [string]$ChecksumFileName
    )

    $expectedHash = Get-ExpectedHash -ChecksumPath $ChecksumPath -FileName $ChecksumFileName
    $actualHash = (Get-FileHash -LiteralPath $FilePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "SHA-256 verification failed for $ChecksumFileName. Expected $expectedHash but found $actualHash."
    }
    return $actualHash
}

function Resolve-PayloadPath {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    $root = [IO.Path]::GetFullPath($payloadRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $relative = $RelativePath.Replace('/', [IO.Path]::DirectorySeparatorChar).Replace('\', [IO.Path]::DirectorySeparatorChar)
    $candidate = [IO.Path]::GetFullPath((Join-Path $payloadRoot $relative))
    if (-not $candidate.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Payload checksum path escapes the payload directory: $RelativePath"
    }
    return $candidate
}

function Assert-PayloadHashes {
    foreach ($name in $requiredPayloadNames) {
        $path = Join-Path $payloadRoot $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "The setup package is incomplete. Missing: $path"
        }
    }

    foreach ($name in ($requiredPayloadNames | Where-Object { $_ -ne 'PAYLOAD-SHA256SUMS.txt' })) {
        [void](Test-FileHash -FilePath (Join-Path $payloadRoot $name) -ChecksumPath $payloadChecksums -ChecksumFileName $name)
    }

    $seen = @{}
    foreach ($line in ((Get-Content -LiteralPath $payloadChecksums -Raw) -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        $match = [regex]::Match($line, '^\s*([a-fA-F0-9]{64})\s+\*?(.+?)\s*$')
        if (-not $match.Success) {
            throw "Invalid PAYLOAD-SHA256SUMS.txt entry: $line"
        }

        $name = $match.Groups[2].Value.Replace('\', '/')
        if ($seen.ContainsKey($name.ToLowerInvariant())) {
            throw "Duplicate PAYLOAD-SHA256SUMS.txt entry: $name"
        }
        $seen[$name.ToLowerInvariant()] = $true

        $path = Resolve-PayloadPath -RelativePath $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "PAYLOAD-SHA256SUMS.txt declares a missing file: $name"
        }

        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        $expectedHash = $match.Groups[1].Value.ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "SHA-256 verification failed for $name. Expected $expectedHash but found $actualHash."
        }
    }
}

function Invoke-SetupStep {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,
        [hashtable]$Parameters = @{}
    )

    Write-Host
    Write-Host $Name -ForegroundColor Cyan
    $parameterJson = ConvertTo-Json -InputObject $Parameters -Compress
    $parameterBase64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($parameterJson))
    $launcher = @'
$ErrorActionPreference = 'Stop'
try {
    $parameterJson = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($env:MPCVR_SETUP_STEP_PARAMS_B64))
    $parameterObject = $parameterJson | ConvertFrom-Json
    $stepParameters = @{}
    foreach ($property in $parameterObject.PSObject.Properties) {
        $stepParameters[$property.Name] = $property.Value
    }
    & $env:MPCVR_SETUP_STEP_SCRIPT @stepParameters *>&1 | ForEach-Object { $_.ToString() }
    exit 0
}
catch {
    Write-Error $_
    exit 1
}
'@
    $previousScript = $env:MPCVR_SETUP_STEP_SCRIPT
    $previousParameters = $env:MPCVR_SETUP_STEP_PARAMS_B64
    try {
        $env:MPCVR_SETUP_STEP_SCRIPT = $ScriptPath
        $env:MPCVR_SETUP_STEP_PARAMS_B64 = $parameterBase64
        & $powerShellExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -Command $launcher
        $stepExitCode = $LASTEXITCODE
    }
    finally {
        $env:MPCVR_SETUP_STEP_SCRIPT = $previousScript
        $env:MPCVR_SETUP_STEP_PARAMS_B64 = $previousParameters
    }
    if ($stepExitCode -ne 0) {
        throw "$Name failed with exit code $stepExitCode."
    }
}

function Get-RifeRuntimeAssetBaseUrl {
    if (-not (Test-Path -LiteralPath $rifeRuntimeSource -PathType Leaf)) {
        throw "RIFE runtime release metadata is missing: $rifeRuntimeSource"
    }

    try {
        $source = Get-Content -LiteralPath $rifeRuntimeSource -Raw | ConvertFrom-Json
    }
    catch {
        throw "RIFE runtime release metadata is invalid JSON: $($_.Exception.Message)"
    }

    $schemaProperty = $source.PSObject.Properties['schemaVersion']
    $tagProperty = $source.PSObject.Properties['releaseTag']
    $urlProperty = $source.PSObject.Properties['assetBaseUrl']
    if (-not $schemaProperty -or [int]$schemaProperty.Value -ne 1) {
        throw 'RIFE runtime release metadata schemaVersion must be 1.'
    }
    if (-not $urlProperty -or [string]::IsNullOrWhiteSpace([string]$urlProperty.Value)) {
        throw 'RIFE runtime release metadata is missing assetBaseUrl.'
    }
    if (-not $tagProperty -or [string]::IsNullOrWhiteSpace([string]$tagProperty.Value)) {
        throw 'RIFE runtime release metadata is missing releaseTag.'
    }

    $uri = $null
    if (-not [Uri]::TryCreate(([string]$urlProperty.Value).TrimEnd('/'), [UriKind]::Absolute, [ref]$uri)) {
        throw 'RIFE runtime assetBaseUrl is not an absolute URL.'
    }
    if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'github.com' -or $uri.AbsolutePath -notmatch '/releases/download/') {
        throw "RIFE runtime assetBaseUrl is not a GitHub HTTPS release URL: $uri"
    }
    $releaseTag = ([string]$tagProperty.Value).Trim()
    if (-not $uri.AbsolutePath.EndsWith("/releases/download/$releaseTag", [StringComparison]::Ordinal)) {
        throw 'RIFE runtime assetBaseUrl does not match its pinned releaseTag.'
    }

    return $uri.AbsoluteUri.TrimEnd('/')
}

function Assert-RifeRuntimeDownloadContract {
    param([Parameter(Mandatory = $true)][string[]]$ArchitectureKeys)

    $checksums = Read-RifeChecksumList -Path $rifeRuntimeChecksums
    $needsDownload = $false
    foreach ($archiveName in @('MPCVR-RIFE-Common.zip') + @($ArchitectureKeys | ForEach-Object { "MPCVR-RIFE-$_.zip" })) {
        if (-not $checksums.ContainsKey($archiveName)) {
            throw "RIFE runtime checksum list is missing $archiveName."
        }
        $hash = [string]$checksums[$archiveName]
        if ($hash -notmatch '^[0-9a-fA-F]{64}$') {
            throw "RIFE runtime checksum is invalid for $archiveName."
        }
        if (($archiveName -ne 'MPCVR-RIFE-Common.zip') -and
                (-not (Test-Path -LiteralPath (Join-Path $payloadRoot $archiveName) -PathType Leaf))) {
            $needsDownload = $true
        }
    }

    if ($needsDownload) {
        return Get-RifeRuntimeAssetBaseUrl
    }
    return $null
}

function New-RifeRuntimeBundle {
    param(
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string[]]$ArchitectureKeys
    )

    $assetBaseUrl = Assert-RifeRuntimeDownloadContract -ArchitectureKeys $ArchitectureKeys
    $checksums = Read-RifeChecksumList -Path $rifeRuntimeChecksums
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null

    $commonArchiveName = 'MPCVR-RIFE-Common.zip'
    $commonArchive = Join-Path $Destination $commonArchiveName
    Copy-Item -LiteralPath (Join-Path $payloadRoot $commonArchiveName) -Destination $commonArchive
    [void](Assert-RifeFileHash -Path $commonArchive -ExpectedHash ([string]$checksums[$commonArchiveName]) -DisplayName $commonArchiveName)

    foreach ($architectureKey in $ArchitectureKeys) {
        $archiveName = "MPCVR-RIFE-$architectureKey.zip"
        $archivePath = Join-Path $Destination $archiveName
        $bundledArchive = Join-Path $payloadRoot $archiveName
        if (Test-Path -LiteralPath $bundledArchive -PathType Leaf) {
            Write-Host "Using bundled RIFE architecture pack $architectureKey..."
            Copy-Item -LiteralPath $bundledArchive -Destination $archivePath
        }
        else {
            $archiveUrl = "$assetBaseUrl/$archiveName"
            Write-Host "Downloading RIFE architecture pack $architectureKey..."
            $request = @{
                Uri = $archiveUrl
                OutFile = $archivePath
            }
            if ($PSVersionTable.PSEdition -eq 'Desktop') {
                $request.UseBasicParsing = $true
            }
            try {
                Invoke-WebRequest @request
            }
            catch {
                throw "Failed to download $archiveName from pinned release ${assetBaseUrl}: $($_.Exception.Message)"
            }
        }
        [void](Assert-RifeFileHash -Path $archivePath -ExpectedHash ([string]$checksums[$archiveName]) -DisplayName $archiveName)
    }

    Copy-Item -LiteralPath $rifeRuntimeManifest -Destination (Join-Path $Destination 'runtime-manifest.json')
    @(
        foreach ($archiveName in @($commonArchiveName) + @($ArchitectureKeys | ForEach-Object { "MPCVR-RIFE-$_.zip" })) {
            "{0}  {1}" -f ([string]$checksums[$archiveName]), $archiveName
        }
    ) | Set-Content -LiteralPath (Join-Path $Destination 'SHA256SUMS.txt') -Encoding ASCII
}

function Assert-MaxinePreflight {
    $runtimePath = Join-Path $env:LOCALAPPDATA 'MPCVR Maxine Runtime\nvvfx\libs'
    foreach ($fileName in @(
        'NVCVImage.dll',
        'NVVideoEffects.dll',
        'nvngxruntime.dll',
        'nvngx_vsr.dll',
        'nvVFXVideoSuperRes.dll'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $runtimePath $fileName) -PathType Leaf)) {
            throw "Maxine preflight is missing runtime file: $fileName"
        }
    }

    $userValue = [Environment]::GetEnvironmentVariable('NV_VIDEO_EFFECTS_PATH', 'User')
    if ([string]::IsNullOrWhiteSpace($userValue)) {
        throw 'Maxine preflight found no user NV_VIDEO_EFFECTS_PATH value.'
    }

    $expectedPath = [IO.Path]::GetFullPath($runtimePath).TrimEnd('\')
    $actualPath = [IO.Path]::GetFullPath($userValue).TrimEnd('\')
    if ($actualPath -ine $expectedPath) {
        throw "Maxine preflight expected NV_VIDEO_EFFECTS_PATH '$expectedPath' but found '$actualPath'."
    }

    return $expectedPath
}

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'MPCVR Maxine + RIFE Setup only supports Windows.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if (-not (Test-Path -LiteralPath $powerShellExecutable -PathType Leaf)) {
    Write-Host "Windows PowerShell 5.1 was not found at $powerShellExecutable." -ForegroundColor Red
    Complete-Run -ExitCode 1
}

if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
    Write-Host 'LOCALAPPDATA is not available.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

$tempRoot = $null
$exitCode = 0
try {
    Assert-PayloadHashes

    Write-Host 'Verified all declared Maxine + RIFE setup payload hashes.' -ForegroundColor Green
    Import-Module $rifeManifestModule -Force

    $gpuInventory = @(Get-RifeGpuInventory -GpuInventoryJson $GpuInventoryJson)
    $architectureKeys = @(Get-RifeArchitectureKeysForInventory -GpuInventory $gpuInventory)
    if ($architectureKeys.Count -eq 0) {
        throw 'No RIFE architecture packs were selected.'
    }
    [void](Assert-RifeRuntimeDownloadContract -ArchitectureKeys $architectureKeys)
    $normalizedGpuInventoryJson = $gpuInventory | ConvertTo-Json -Compress
    Write-Host ('Detected NVIDIA GPUs: {0}' -f (($gpuInventory | ForEach-Object { '{0} (CC {1})' -f $_.Name, $_.ComputeCapability }) -join '; '))
    Write-Host ('RIFE architecture packs: {0}' -f ($architectureKeys -join ', '))

    if ($ValidateOnly) {
        Invoke-SetupStep -Name 'Validating the Maxine runtime installer...' -ScriptPath $maxineRuntimeInstaller -Parameters @{
            ValidateOnly = $true
            NoPause = $true
        }

        $rifeValidationParameters = @{
            ValidateOnly = $true
            NoPause = $true
            GpuInventoryJson = $normalizedGpuInventoryJson
        }
        Invoke-SetupStep -Name 'Validating the RIFE runtime installer...' -ScriptPath $rifeRuntimeInstaller -Parameters $rifeValidationParameters

        Invoke-SetupStep `
            -Name 'Validating the renderer updater...' `
            -ScriptPath $rendererUpdater `
            -Parameters @{
                PackageArchive = $rendererArchive
                ChecksumFile = $payloadChecksums
                ValidateOnly = $true
                NoPause = $true
            }

        Write-Host
        Write-Host 'Unified Maxine + RIFE setup validation passed.' -ForegroundColor Green
        Complete-Run -ExitCode 0
    }

    if (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
        Write-Host 'MPC-HC is open. Close it to continue installation automatically (Ctrl+C to cancel).' -ForegroundColor Yellow
        while (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue) {
            Start-Sleep -Milliseconds 500
        }
        Write-Host 'MPC-HC has closed. Continuing installation...' -ForegroundColor Green
    }

    if ($PSVersionTable.PSEdition -eq 'Desktop') {
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    }

    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("MPCVR-Maxine-RIFE-Setup-{0}" -f [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

    $stagedMaxineArchive = Join-Path $tempRoot 'MPCVR-Maxine-Runtime.zip'
    Copy-Item -LiteralPath $maxineRuntimeArchive -Destination $stagedMaxineArchive
    $maxineHash = Get-ExpectedHash -ChecksumPath $payloadChecksums -FileName 'MPCVR-Maxine-Runtime.zip'
    "$maxineHash  MPCVR-Maxine-Runtime.zip" | Set-Content -LiteralPath "$stagedMaxineArchive.sha256" -Encoding ASCII

    $rifeBundleRoot = Join-Path $tempRoot 'rife-runtime'
    New-RifeRuntimeBundle -Destination $rifeBundleRoot -ArchitectureKeys $architectureKeys

    Invoke-SetupStep `
        -Name '1/4 Installing NVIDIA Maxine runtime...' `
        -ScriptPath $maxineRuntimeInstaller `
        -Parameters @{
            RuntimeArchive = $stagedMaxineArchive
            NoPause = $true
        }

    $rifeInstallParameters = @{
        RuntimeBundleRoot = $rifeBundleRoot
        ModelArchive = $rifeModelArchive
        NoPause = $true
        GpuInventoryJson = $normalizedGpuInventoryJson
    }
    Invoke-SetupStep `
        -Name '2/4 Installing RIFE TensorRT runtime and models...' `
        -ScriptPath $rifeRuntimeInstaller `
        -Parameters $rifeInstallParameters

    Invoke-SetupStep `
        -Name '3/4 Installing K-Lite restore shortcut...' `
        -ScriptPath $updaterInstaller `
        -Parameters @{ NoPause = $true }

    Invoke-SetupStep `
        -Name '4/4 Installing custom MPC Video Renderer into K-Lite...' `
        -ScriptPath $rendererUpdater `
        -Parameters @{
            PackageArchive = $rendererArchive
            ChecksumFile = $payloadChecksums
            NoPause = $true
        }

    $rifeInstallRoot = Join-Path $env:LOCALAPPDATA 'MPCVideoRenderer\RIFE'
    $rifePreflightParameters = @{
        InstallRoot = $rifeInstallRoot
        GpuInventoryJson = $normalizedGpuInventoryJson
    }
    Invoke-SetupStep -Name 'Running RIFE post-install preflight...' -ScriptPath $rifePreflight -Parameters $rifePreflightParameters
    $maxineRuntimePath = Assert-MaxinePreflight

    Write-Host
    Write-Host 'MPCVR Maxine + RIFE Setup completed successfully.' -ForegroundColor Green
    Write-Host
    Write-Host 'Open MPC-HC and play a video.'
    Write-Host 'Press Ctrl+J and confirm:'
    Write-Host "  Maxine runtime: $maxineRuntimePath"
    Write-Host '  RIFE runtime: ready'
    Write-Host '  RIFE models: 4.4, 4.6, 4.15 Lite'
    Write-Host 'First playback may build a TensorRT engine under %LOCALAPPDATA%\MPCVideoRenderer\RIFE\cache.'
}
catch {
    $exitCode = 1
    Write-Host
    Write-Host "Setup failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}
finally {
    if ($tempRoot -and (Test-Path -LiteralPath $tempRoot)) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Complete-Run -ExitCode $exitCode
