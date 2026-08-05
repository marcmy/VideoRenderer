#requires -Version 5.1

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$modulePath = Join-Path $PSScriptRoot 'MpcvrNvOffruc.Acquisition.psm1'
Import-Module -Name $modulePath -Force

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('MPCVR-NvOFFRUC-Acquisition-Test-' + [guid]::NewGuid().ToString('N'))
try {
    $downloads = Join-Path $tempRoot 'Downloads'
    $cache = Join-Path $tempRoot 'Cache'
    $sdkRoot = Join-Path $tempRoot 'SdkRoot'
    $runtimeDir = Join-Path $sdkRoot 'Optical_Flow_SDK_5.0.7\NvOFFRUC\NvOFFRUCSample\bin\win64'
    New-Item -ItemType Directory -Path $downloads, $cache, $runtimeDir -Force | Out-Null

    Set-Content -LiteralPath (Join-Path $runtimeDir 'NvOFFRUC.dll') -Value 'synthetic-nvoffruc' -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $runtimeDir 'cudart64_110.dll') -Value 'synthetic-cudart' -Encoding ASCII

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $sdkZip = Join-Path $downloads 'Optical_Flow_SDK_5.0.7.zip'
    [IO.Compression.ZipFile]::CreateFromDirectory($sdkRoot, $sdkZip)

    Assert-True `
        -Condition (Test-MpcvrOpticalFlowSdkArchive -Path $sdkZip) `
        -Message 'The compatible synthetic SDK ZIP was not recognized.'

    $invalidZip = Join-Path $downloads 'Optical_Flow_invalid.zip'
    $invalidRoot = Join-Path $tempRoot 'InvalidRoot'
    New-Item -ItemType Directory -Path $invalidRoot -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $invalidRoot 'not-the-sdk.txt') -Value 'invalid' -Encoding ASCII
    [IO.Compression.ZipFile]::CreateFromDirectory($invalidRoot, $invalidZip)
    Assert-True `
        -Condition (-not (Test-MpcvrOpticalFlowSdkArchive -Path $invalidZip)) `
        -Message 'An incompatible ZIP was accepted as the Optical Flow SDK.'

    $found = Find-MpcvrOpticalFlowSdkArchive -SearchDirectories @($downloads)
    Assert-True `
        -Condition ([IO.Path]::GetFullPath($found) -eq [IO.Path]::GetFullPath($sdkZip)) `
        -Message 'Automatic SDK discovery did not return the compatible ZIP.'

    $resolved = Resolve-MpcvrOpticalFlowSdkPackage `
        -CacheRoot $downloads `
        -DisableOfficialDownload
    Assert-True `
        -Condition ([IO.Path]::GetFullPath($resolved) -eq [IO.Path]::GetFullPath($sdkZip)) `
        -Message 'Existing-package resolution did not avoid the browser/picker path.'

    $cached = Copy-MpcvrOpticalFlowSdkToCache -SourcePath $sdkZip -CacheRoot $cache
    Assert-True -Condition (Test-Path -LiteralPath $cached -PathType Leaf) -Message 'SDK caching did not create a file.'
    Assert-True -Condition (Test-MpcvrOpticalFlowSdkArchive -Path $cached) -Message 'The cached SDK failed validation.'

    $hashRejected = $false
    try {
        [void](Test-MpcvrNvOffrucRuntimeHashes `
            -NvOffrucPath (Join-Path $runtimeDir 'NvOFFRUC.dll') `
            -CudaRuntimePath (Join-Path $runtimeDir 'cudart64_110.dll'))
    }
    catch {
        $hashRejected = $true
    }
    Assert-True -Condition $hashRejected -Message 'Tampered runtime files were not rejected.'

    Assert-True `
        -Condition (Test-MpcvrNvOffrucRuntimeHashes `
            -NvOffrucPath (Join-Path $runtimeDir 'NvOFFRUC.dll') `
            -CudaRuntimePath (Join-Path $runtimeDir 'cudart64_110.dll') `
            -AllowUnverifiedRuntimeFiles) `
        -Message 'The explicit Advanced override did not permit synthetic runtime files.'

    $url = Get-MpcvrOpticalFlowOfficialDownloadUrl
    Assert-True `
        -Condition ($url -match '^https://developer\.nvidia\.com/.+optical_flow_sdk_5\.0\.7\.zip') `
        -Message 'The official NVIDIA SDK URL is missing or malformed.'

    Write-Host 'NvOFFRUC assisted acquisition tests passed.' -ForegroundColor Green
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
