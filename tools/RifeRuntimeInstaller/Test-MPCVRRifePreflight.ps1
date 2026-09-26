#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallRoot,
    [string]$GpuInventoryJson,
    [switch]$ThrowOnFailure
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'RifeRuntimeManifest.psm1') -Force

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

function Initialize-RifePreflightNativeHelper {
    if ('MpcVrRifePreflight.NativeMethods' -as [type]) {
        return
    }

    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace MpcVrRifePreflight
{
    public static class NativeMethods
    {
        private const uint LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x00000100;
        private const uint LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryExW(string lpFileName, IntPtr hFile, uint dwFlags);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FreeLibrary(IntPtr hModule);

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate UInt32 GetAbiVersionDelegate();

        public static UInt32 GetAbiVersion(string dllPath)
        {
            IntPtr module = LoadLibraryExW(
                dllPath,
                IntPtr.Zero,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (module == IntPtr.Zero)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "LoadLibraryExW failed for " + dllPath);
            }

            try
            {
                IntPtr address = GetProcAddress(module, "MpcvrRifeGetAbiVersion");
                if (address == IntPtr.Zero)
                {
                    throw new MissingMethodException("MpcvrRifeGetAbiVersion export was not found in " + dllPath);
                }

                var callback = (GetAbiVersionDelegate)Marshal.GetDelegateForFunctionPointer(
                    address,
                    typeof(GetAbiVersionDelegate));
                return callback();
            }
            finally
            {
                FreeLibrary(module);
            }
        }
    }
}
"@
}

function Invoke-RifePreflight {
    $root = [IO.Path]::GetFullPath($InstallRoot)
    $runtimeRoot = Join-Path $root 'runtime'
    $cacheRoot = Join-Path $root 'cache'
    $manifestPath = Join-Path $root 'installed-manifest.json'

    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Installed RIFE manifest was not found: $manifestPath"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([int](Get-RequiredProperty $manifest 'schemaVersion' 'installed manifest') -ne 2) {
        throw 'Installed RIFE manifest schemaVersion must be 2.'
    }
    if ([int](Get-RequiredProperty $manifest 'runtimeAbi' 'installed manifest') -ne 2) {
        throw 'Installed RIFE manifest runtimeAbi must be 2.'
    }

    foreach ($fileName in @(
        'MPCVRRifeRuntime64.dll',
        'nvinfer_11.dll',
        'nvonnxparser_11.dll',
        'nvinfer_builder_resource_ptx_11.dll'
    )) {
        $path = Join-Path $runtimeRoot $fileName
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "RIFE preflight is missing runtime file: $fileName"
        }
    }

    $gpuInventory = @(Get-RifeGpuInventory -GpuInventoryJson $GpuInventoryJson)
    $architectureKeys = @(Get-RifeArchitectureKeysForInventory -GpuInventory $gpuInventory)
    $installedArchitectures = @(Get-RequiredProperty $manifest 'architectures' 'installed manifest')
    foreach ($key in $architectureKeys) {
        $resource = Get-RifeBuilderResourceName -Architecture $key
        if (-not (Test-Path -LiteralPath (Join-Path $runtimeRoot $resource) -PathType Leaf)) {
            throw "RIFE preflight is missing builder resource for ${key}: $resource"
        }
        $declared = @($installedArchitectures | Where-Object { [string]$_.key -eq $key })
        if ($declared.Count -ne 1) {
            throw "Installed RIFE manifest does not declare selected architecture '$key' exactly once."
        }
    }

    $expectedModels = @(
        [pscustomobject]@{ Model = 'RIFE 4.4'; File = 'models/rife_v4.4.onnx' },
        [pscustomobject]@{ Model = 'RIFE 4.6'; File = 'models/rife_v4.6.onnx' },
        [pscustomobject]@{ Model = 'RIFE 4.15 Lite'; File = 'models/rife_v4.15_lite.onnx' }
    )
    $installedModels = @(Get-RequiredProperty $manifest 'models' 'installed manifest')
    if ($installedModels.Count -ne $expectedModels.Count) {
        throw "Installed RIFE manifest must contain exactly $($expectedModels.Count) models."
    }
    $modelSummaries = @()
    foreach ($expected in $expectedModels) {
        $matches = @($installedModels | Where-Object {
            [string]$_.model -eq $expected.Model -and [string]$_.modelFile -eq $expected.File
        })
        if ($matches.Count -ne 1) {
            throw "Installed RIFE manifest must declare $($expected.Model) as $($expected.File) exactly once."
        }
        $entry = $matches[0]
        $modelPath = Join-Path $root $expected.File.Replace('/', [IO.Path]::DirectorySeparatorChar)
        if (-not (Test-Path -LiteralPath $modelPath -PathType Leaf)) {
            throw "RIFE preflight is missing model: $modelPath"
        }
        $expectedModelHash = [string](Get-RequiredProperty $entry 'modelSha256' "$($expected.Model) installed manifest entry")
        [void](Assert-RifeFileHash -Path $modelPath -ExpectedHash $expectedModelHash -DisplayName $expected.File)
        $modelSummaries += "$($expected.Model) $($expectedModelHash.Substring(0, 12))"
    }

    if (-not (Test-Path -LiteralPath $cacheRoot -PathType Container)) {
        throw "RIFE preflight is missing cache directory: $cacheRoot"
    }
    $writeProbe = Join-Path $cacheRoot ('.write-test-{0}.tmp' -f [guid]::NewGuid().ToString('N'))
    try {
        Set-Content -LiteralPath $writeProbe -Value 'write-test' -Encoding ASCII
        if (-not (Test-Path -LiteralPath $writeProbe -PathType Leaf)) {
            throw 'RIFE cache write test did not create its probe file.'
        }
    }
    finally {
        Remove-Item -LiteralPath $writeProbe -Force -ErrorAction SilentlyContinue
    }

    Initialize-RifePreflightNativeHelper
    $runtimeDll = Join-Path $runtimeRoot 'MPCVRRifeRuntime64.dll'
    $abi = [MpcVrRifePreflight.NativeMethods]::GetAbiVersion($runtimeDll)
    if ($abi -ne 2) {
        throw "MPCVRRifeRuntime64.dll reports unsupported ABI $abi; expected 2."
    }

    Write-Host 'RIFE runtime preflight passed.' -ForegroundColor Green
    Write-Host ('GPUs: {0}' -f (($gpuInventory | ForEach-Object { '{0} (CC {1})' -f $_.Name, $_.ComputeCapability }) -join '; '))
    Write-Host ('Architecture packs: {0}' -f ($architectureKeys -join ', '))
    Write-Host ('TensorRT: {0}' -f [string](Get-RequiredProperty $manifest 'tensorRtVersion' 'installed manifest'))
    Write-Host ('Models: {0}' -f ($modelSummaries -join '; '))
    Write-Host ('Runtime ABI: {0}' -f $abi)
}

try {
    Invoke-RifePreflight
    if (-not $ThrowOnFailure) {
        exit 0
    }
}
catch {
    if ($ThrowOnFailure) {
        throw
    }

    Write-Host "RIFE runtime preflight failed: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage -ForegroundColor DarkGray
    }
    exit 1
}
