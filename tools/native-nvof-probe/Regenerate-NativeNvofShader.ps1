param(
    [switch] $Check
)

$ErrorActionPreference = 'Stop'

$sourcePath = 'Source/NvidiaOpticalFlowNative.cpp'
$headerPath = 'Source/NvidiaOpticalFlowMidpointBytecode.h'

$sdkBin = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$fxc = Get-ChildItem $sdkBin -Filter fxc.exe -Recurse -File |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $fxc) {
    throw 'fxc.exe was not found in the Windows SDK.'
}

$source = [IO.File]::ReadAllText($sourcePath)
$startMarker = 'constexpr char MidpointShader[] = R"hlsl('
$endMarker = ')hlsl";'
$start = $source.IndexOf($startMarker)
if ($start -lt 0) { throw 'MidpointShader start marker was not found.' }
$start += $startMarker.Length
$end = $source.IndexOf($endMarker, $start)
if ($end -lt 0) { throw 'MidpointShader end marker was not found.' }

$tempDir = Join-Path $env:TEMP ('mpcvr-nvof-shader-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempDir | Out-Null
try {
    $hlslPath = Join-Path $tempDir 'NativeNvofMidpoint.hlsl'
    $generatedHeader = Join-Path $tempDir 'NvidiaOpticalFlowMidpointBytecode.h'
    [IO.File]::WriteAllText($hlslPath, $source.Substring($start, $end - $start))

    & $fxc.FullName /nologo /T cs_5_0 /E main /O3 /WX `
        /Fh $generatedHeader /Vn g_NativeNvofMidpointBytecode $hlslPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if ($Check) {
        if (-not (Test-Path $headerPath)) {
            throw "$headerPath does not exist. Run this script without -Check to generate it."
        }
        $expected = [IO.File]::ReadAllBytes($generatedHeader)
        $actual = [IO.File]::ReadAllBytes($headerPath)
        if (-not $expected.SequenceEqual($actual)) {
            throw 'Native NVOF embedded shader bytecode is stale. Run tools/native-nvof-probe/Regenerate-NativeNvofShader.ps1 and commit the regenerated header.'
        }
        Write-Host 'Native NVOF embedded shader bytecode matches MidpointShader.'
    }
    else {
        Copy-Item $generatedHeader $headerPath -Force
        Write-Host "Regenerated $headerPath"
    }
}
finally {
    Remove-Item $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
