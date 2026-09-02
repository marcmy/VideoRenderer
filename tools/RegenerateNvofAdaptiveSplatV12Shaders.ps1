$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo 'Source'

$fxc = Get-Command fxc.exe -ErrorAction SilentlyContinue
if (-not $fxc) {
    $kits = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'),
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\8.1\bin')
    ) | Where-Object { Test-Path $_ }

    foreach ($kit in $kits) {
        $candidate = Get-ChildItem -LiteralPath $kit -Filter fxc.exe -File -Recurse -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($candidate) {
            $fxc = $candidate
            break
        }
    }
}

if (-not $fxc) {
    throw 'fxc.exe was not found; cannot regenerate adaptive NVOF shader bytecode.'
}

$fxcPath = if ($fxc.Source) { $fxc.Source } else { $fxc.FullName }
Write-Host "Using fxc: $fxcPath"

$shaders = @(
    @{
        Source = 'NvidiaOpticalFlowDenseSeed.hlsl'
        Header = 'NvidiaOpticalFlowDenseSeedBytecode.h'
        Symbol = 'g_NvofDenseSeedBytecode'
    },
    @{
        Source = 'NvidiaOpticalFlowDenseRegionGate.hlsl'
        Header = 'NvidiaOpticalFlowDenseRegionGateBytecode.h'
        Symbol = 'g_NvofDenseRegionGateBytecode'
    },
    @{
        Source = 'NvidiaOpticalFlowAdaptiveSplatScatter.hlsl'
        Header = 'NvidiaOpticalFlowAdaptiveSplatScatterBytecode.h'
        Symbol = 'g_NvofAdaptiveSplatScatterBytecode'
    },
    @{
        Source = 'NvidiaOpticalFlowAdaptiveSplatResolve.hlsl'
        Header = 'NvidiaOpticalFlowAdaptiveSplatResolveBytecode.h'
        Symbol = 'g_NvofAdaptiveSplatResolveBytecode'
    },
    @{
        Source = 'NvidiaOpticalFlowDenseWarp.hlsl'
        Header = 'NvidiaOpticalFlowDenseWarpBytecode.h'
        Symbol = 'g_NvofDenseWarpBytecode'
    }
)

foreach ($shader in $shaders) {
    $inputPath = Join-Path $source $shader.Source
    $outputPath = Join-Path $source $shader.Header
    Write-Host "Compiling $($shader.Source) -> $($shader.Header)"
    & $fxcPath /nologo /T cs_5_0 /E main /WX /O3 /Fh $outputPath /Vn $shader.Symbol $inputPath
    if ($LASTEXITCODE -ne 0) {
        throw "fxc failed for $($shader.Source) with exit code $LASTEXITCODE"
    }
}

Write-Host 'Regenerated adaptive-splat V1.2 native NVOF shader bytecode.'
