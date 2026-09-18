#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$RuntimeBundleRoot,
    [string]$ModelArchive,
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'MPCVideoRenderer\RIFE'),
    [string]$GpuInventoryJson,
    [switch]$NoPause,
    [switch]$ValidateOnly,
    [switch]$TestInjectInvalidInstalledModelHash
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

Import-Module (Join-Path $PSScriptRoot 'RifeRuntimeManifest.psm1') -Force

function Complete-Run {
    param([int]$ExitCode)

    if (-not $NoPause) {
        Write-Host
        [void](Read-Host 'Press Enter to close')
    }
    exit $ExitCode
}

function Get-RequiredProperty {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $property = $Object.PSObject.Properties[$Name]
    if (-not $property) {
        throw "$Context is missing required property '$Name'."
    }
    $property.Value
}

function Resolve-PackageFile {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $relative = $RelativePath.Replace('/', [IO.Path]::DirectorySeparatorChar).Replace('\', [IO.Path]::DirectorySeparatorChar)
    $candidate = [IO.Path]::GetFullPath((Join-Path $Root $relative))
    if (-not $candidate.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package checksum path escapes its package root: $RelativePath"
    }
    $candidate
}

function Test-ModelPackage {
    param([Parameter(Mandatory = $true)][string]$Root)

    $checksumPath = Join-Path $Root 'SHA256SUMS.txt'
    $manifestPath = Join-Path $Root 'model-manifest.json'
    $modelPath = Join-Path $Root 'rife_v4.6.onnx'
    foreach ($required in @($checksumPath, $manifestPath, $modelPath)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "RIFE model package is missing required file: $required"
        }
    }

    $checksums = Read-RifeChecksumList -Path $checksumPath
    foreach ($name in @($checksums.Keys)) {
        $packageFile = Resolve-PackageFile -Root $Root -RelativePath $name
        [void](Assert-RifeFileHash -Path $packageFile -ExpectedHash ([string]$checksums[$name]) -DisplayName $name)
    }

    if (-not $checksums.ContainsKey('rife_v4.6.onnx')) {
        throw 'RIFE model SHA256SUMS.txt does not contain rife_v4.6.onnx.'
    }
    if (-not $checksums.ContainsKey('model-manifest.json')) {
        throw 'RIFE model SHA256SUMS.txt does not contain model-manifest.json.'
    }

    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([int](Get-RequiredProperty $manifest 'schemaVersion' 'model-manifest.json') -ne 1) {
        throw 'RIFE model manifest schemaVersion must be 1.'
    }
    if ([string](Get-RequiredProperty $manifest 'model' 'model-manifest.json') -ne 'RIFE 4.6') {
        throw 'RIFE model manifest model must be RIFE 4.6.'
    }
    if ([string](Get-RequiredProperty $manifest 'file' 'model-manifest.json') -ne 'rife_v4.6.onnx') {
        throw 'RIFE model manifest file must be rife_v4.6.onnx.'
    }
    if ([string](Get-RequiredProperty $manifest 'precision' 'model-manifest.json') -ne 'mixed-fp16-fp32') {
        throw 'RIFE model manifest precision must be mixed-fp16-fp32.'
    }
    $sourceArchiveSha256 = [string](Get-RequiredProperty $manifest 'sourceArchiveSha256' 'model-manifest.json')
    if ($sourceArchiveSha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'RIFE model manifest sourceArchiveSha256 is invalid.'
    }

    [pscustomobject]@{
        Manifest = $manifest
        ModelPath = $modelPath
        ModelSha256 = [string]$checksums['rife_v4.6.onnx']
    }
}

function Test-CacheCompatibility {
    param(
        [Parameter(Mandatory = $true)]$NewManifest,
        [Parameter(Mandatory = $true)][string]$ExistingManifestPath
    )

    if (-not (Test-Path -LiteralPath $ExistingManifestPath -PathType Leaf)) {
        return $false
    }

    try {
        $old = Get-Content -LiteralPath $ExistingManifestPath -Raw | ConvertFrom-Json
        $oldModel = [string](Get-RequiredProperty $old 'modelSha256' 'installed manifest')
        $oldTensorRt = [string](Get-RequiredProperty $old 'tensorRtVersion' 'installed manifest')
        $oldAbi = [int](Get-RequiredProperty $old 'runtimeAbi' 'installed manifest')
        return (
            $oldModel -eq [string]$NewManifest.modelSha256 -and
            (Get-RifeTensorRtMajorMinor -Version $oldTensorRt) -eq [string]$NewManifest.tensorRtMajorMinor -and
            $oldAbi -eq [int]$NewManifest.runtimeAbi
        )
    }
    catch {
        Write-Warning "Existing RIFE cache will not be reused because its installed manifest is incompatible: $($_.Exception.Message)"
        return $false
    }
}

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'This installer only supports Windows.' -ForegroundColor Red
    Complete-Run -ExitCode 1
}

$exitCode = 0
$tempRoot = $null
$stagingRoot = $null
$backupRoot = $null
$existingMoved = $false
$newInstalled = $false

try {
    $gpuInventory = @(Get-RifeGpuInventory -GpuInventoryJson $GpuInventoryJson)
    $architectureKeys = @(Get-RifeArchitectureKeysForInventory -GpuInventory $gpuInventory)
    Write-Host ('Detected NVIDIA GPUs: {0}' -f (($gpuInventory | ForEach-Object { '{0} (CC {1})' -f $_.Name, $_.ComputeCapability }) -join '; '))
    Write-Host ('Selected architecture packs: {0}' -f ($architectureKeys -join ', '))

    if ($ValidateOnly) {
        if ($architectureKeys.Count -eq 0) {
            throw 'No RIFE architecture packs were selected.'
        }
        Write-Host 'RIFE runtime installer validation passed.' -ForegroundColor Green
        Complete-Run -ExitCode 0
    }

    if ([string]::IsNullOrWhiteSpace($RuntimeBundleRoot)) {
        throw '-RuntimeBundleRoot is required for installation.'
    }
    if ([string]::IsNullOrWhiteSpace($ModelArchive)) {
        throw '-ModelArchive is required for installation.'
    }
    if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
        throw '-InstallRoot resolved to an empty path.'
    }

    $RuntimeBundleRoot = (Resolve-Path -LiteralPath $RuntimeBundleRoot).Path
    $ModelArchive = (Resolve-Path -LiteralPath $ModelArchive).Path
    $InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
    $stagingRoot = "$InstallRoot.staging-$([guid]::NewGuid().ToString('N'))"
    $backupRoot = "$InstallRoot.backup"
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("MPCVR-RIFE-Install-{0}" -f [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

    $runtimeManifestPath = Join-Path $RuntimeBundleRoot 'runtime-manifest.json'
    $runtimeChecksumsPath = Join-Path $RuntimeBundleRoot 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $runtimeManifestPath -PathType Leaf)) {
        throw "RIFE runtime bundle is missing runtime-manifest.json: $RuntimeBundleRoot"
    }
    $runtimeManifest = Get-Content -LiteralPath $runtimeManifestPath -Raw | ConvertFrom-Json
    if ([int](Get-RequiredProperty $runtimeManifest 'schemaVersion' 'runtime-manifest.json') -ne 1) {
        throw 'RIFE runtime manifest schemaVersion must be 1.'
    }
    if ([int](Get-RequiredProperty $runtimeManifest 'runtimeAbi' 'runtime-manifest.json') -ne 2) {
        throw 'RIFE runtime manifest runtimeAbi must be 2.'
    }
    $tensorRtVersion = [string](Get-RequiredProperty $runtimeManifest 'tensorRtVersion' 'runtime-manifest.json')
    $tensorRtMajorMinor = Get-RifeTensorRtMajorMinor -Version $tensorRtVersion
    $runtimeChecksums = Read-RifeChecksumList -Path $runtimeChecksumsPath

    $declaredArchitectures = @(Get-RequiredProperty $runtimeManifest 'architectures' 'runtime-manifest.json')
    foreach ($key in $architectureKeys) {
        $matches = @($declaredArchitectures | Where-Object { [string]$_.key -eq $key })
        if ($matches.Count -ne 1) {
            throw "RIFE runtime manifest must declare selected architecture '$key' exactly once."
        }
        $expectedResource = Get-RifeBuilderResourceName -Architecture $key
        if ([string]$matches[0].builderResource -ne $expectedResource) {
            throw "RIFE runtime manifest builder resource mismatch for '$key'."
        }
    }

    $commonArchiveName = 'MPCVR-RIFE-Common.zip'
    if (-not $runtimeChecksums.ContainsKey($commonArchiveName)) {
        throw "RIFE runtime checksum list is missing $commonArchiveName."
    }
    $commonArchive = Join-Path $RuntimeBundleRoot $commonArchiveName
    $commonHash = Assert-RifeFileHash -Path $commonArchive -ExpectedHash ([string]$runtimeChecksums[$commonArchiveName]) -DisplayName $commonArchiveName

    $architectureInfo = @()
    foreach ($key in $architectureKeys) {
        $archiveName = "MPCVR-RIFE-$key.zip"
        if (-not $runtimeChecksums.ContainsKey($archiveName)) {
            throw "RIFE runtime checksum list is missing $archiveName."
        }
        $archivePath = Join-Path $RuntimeBundleRoot $archiveName
        $archiveHash = Assert-RifeFileHash -Path $archivePath -ExpectedHash ([string]$runtimeChecksums[$archiveName]) -DisplayName $archiveName
        $architectureInfo += [pscustomobject][ordered]@{
            key = $key
            archive = $archiveName
            sha256 = $archiveHash
            builderResource = (Get-RifeBuilderResourceName -Architecture $key)
        }
    }

    $modelRoot = $null
    if (Test-Path -LiteralPath $ModelArchive -PathType Container) {
        $modelRoot = $ModelArchive
    }
    else {
        $modelRoot = Join-Path $tempRoot 'model'
        New-Item -ItemType Directory -Path $modelRoot -Force | Out-Null
        Expand-Archive -LiteralPath $ModelArchive -DestinationPath $modelRoot -Force
    }
    $modelPackage = Test-ModelPackage -Root $modelRoot

    $stagedRuntime = Join-Path $stagingRoot 'runtime'
    $stagedModels = Join-Path $stagingRoot 'models'
    $stagedCache = Join-Path $stagingRoot 'cache'
    New-Item -ItemType Directory -Path $stagedRuntime, $stagedModels, $stagedCache -Force | Out-Null
    Expand-Archive -LiteralPath $commonArchive -DestinationPath $stagedRuntime -Force
    foreach ($entry in $architectureInfo) {
        Expand-Archive -LiteralPath (Join-Path $RuntimeBundleRoot $entry.archive) -DestinationPath $stagedRuntime -Force
    }

    foreach ($fileName in @(Get-RifeCommonRuntimeFileNames)) {
        if (-not (Test-Path -LiteralPath (Join-Path $stagedRuntime $fileName) -PathType Leaf)) {
            throw "Staged RIFE runtime is missing $fileName."
        }
    }
    foreach ($entry in $architectureInfo) {
        if (-not (Test-Path -LiteralPath (Join-Path $stagedRuntime $entry.builderResource) -PathType Leaf)) {
            throw "Staged RIFE runtime is missing $($entry.builderResource)."
        }
    }

    $stagedModel = Join-Path $stagedModels 'rife_v4.6.onnx'
    Copy-Item -LiteralPath $modelPackage.ModelPath -Destination $stagedModel -Force
    [void](Assert-RifeFileHash -Path $stagedModel -ExpectedHash $modelPackage.ModelSha256 -DisplayName 'models/rife_v4.6.onnx')

    $installedManifest = [pscustomobject][ordered]@{
        schemaVersion = 1
        runtimeAbi = [int]$runtimeManifest.runtimeAbi
        tensorRtVersion = $tensorRtVersion
        tensorRtMajorMinor = $tensorRtMajorMinor
        cudaVersion = [string](Get-RequiredProperty $runtimeManifest 'cudaVersion' 'runtime-manifest.json')
        cudaRuntimeVersion = [string](Get-RequiredProperty $runtimeManifest 'cudaRuntimeVersion' 'runtime-manifest.json')
        commonRuntimeArchiveSha256 = $commonHash
        architectures = $architectureInfo
        model = [string]$modelPackage.Manifest.model
        modelFile = 'models/rife_v4.6.onnx'
        modelSha256 = $modelPackage.ModelSha256
        sourceRelease = [string](Get-RequiredProperty $modelPackage.Manifest 'sourceRelease' 'model-manifest.json')
        sourceArchiveSha256 = [string]$modelPackage.Manifest.sourceArchiveSha256
    }
    $stagedManifestPath = Join-Path $stagingRoot 'installed-manifest.json'
    $installedManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $stagedManifestPath -Encoding UTF8

    $existingManifestPath = Join-Path $InstallRoot 'installed-manifest.json'
    if (Test-CacheCompatibility -NewManifest $installedManifest -ExistingManifestPath $existingManifestPath) {
        $existingCache = Join-Path $InstallRoot 'cache'
        if (Test-Path -LiteralPath $existingCache -PathType Container) {
            Get-ChildItem -LiteralPath $existingCache -Filter '*.plan' -File -ErrorAction SilentlyContinue |
                Copy-Item -Destination $stagedCache -Force
            Write-Host 'Preserved compatible TensorRT engine cache.'
        }
    }

    if (Test-Path -LiteralPath $backupRoot) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $InstallRoot) {
        Move-Item -LiteralPath $InstallRoot -Destination $backupRoot
        $existingMoved = $true
    }

    Move-Item -LiteralPath $stagingRoot -Destination $InstallRoot
    $newInstalled = $true

    if ($TestInjectInvalidInstalledModelHash) {
        $testManifestPath = Join-Path $InstallRoot 'installed-manifest.json'
        $testManifest = Get-Content -LiteralPath $testManifestPath -Raw | ConvertFrom-Json
        $testManifest.modelSha256 = ('0' * 64)
        $testManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $testManifestPath -Encoding UTF8
    }

    $preflightPath = Join-Path $PSScriptRoot 'Test-MPCVRRifePreflight.ps1'
    & $preflightPath -InstallRoot $InstallRoot -GpuInventoryJson ($gpuInventory | ConvertTo-Json -Compress) -ThrowOnFailure

    if ($existingMoved -and (Test-Path -LiteralPath $backupRoot)) {
        Remove-Item -LiteralPath $backupRoot -Recurse -Force
        $existingMoved = $false
    }

    Write-Host
    Write-Host 'MPC-VR RIFE TensorRT runtime installed successfully.' -ForegroundColor Green
    Write-Host "Install root: $InstallRoot"
    Write-Host ('Architecture packs: {0}' -f ($architectureKeys -join ', '))
}
catch {
    $exitCode = 1

    if ($newInstalled -and (Test-Path -LiteralPath $InstallRoot)) {
        Remove-Item -LiteralPath $InstallRoot -Recurse -Force -ErrorAction SilentlyContinue
        $newInstalled = $false
    }
    if ($existingMoved -and (Test-Path -LiteralPath $backupRoot)) {
        Move-Item -LiteralPath $backupRoot -Destination $InstallRoot -ErrorAction SilentlyContinue
        $existingMoved = $false
    }

    Write-Host
    Write-Host "RIFE runtime installation failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
}
finally {
    if ($tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Complete-Run -ExitCode $exitCode
