[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BundleRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-EqualSet {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Actual,
        [Parameter(Mandatory = $true)]
        [string[]]$Expected,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $actualSorted = @($Actual | Sort-Object -Unique)
    $expectedSorted = @($Expected | Sort-Object -Unique)
    $difference = @(Compare-Object -ReferenceObject $expectedSorted -DifferenceObject $actualSorted)
    if ($difference.Count -ne 0) {
        $rendered = $difference | ForEach-Object {
            $direction = if ($_.SideIndicator -eq '=>') { 'unexpected' } else { 'missing' }
            "$direction '$($_.InputObject)'"
        }
        throw "$Description payload mismatch: $($rendered -join ', ')"
    }
}

function Get-ZipFileEntries {
    param([Parameter(Mandatory = $true)][string]$ArchivePath)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        @(
            $archive.Entries |
                Where-Object { -not [string]::IsNullOrEmpty($_.Name) } |
                ForEach-Object { $_.FullName.Replace('\', '/') }
        )
    }
    finally {
        $archive.Dispose()
    }
}

function Assert-Manifest {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][object[]]$ExpectedArchitectures
    )

    if ([int]$Manifest.schemaVersion -ne 1) {
        throw "runtime-manifest.json has unsupported schemaVersion '$($Manifest.schemaVersion)'"
    }
    if ([string]$Manifest.cudaVersion -ne '13.4.1') {
        throw "runtime-manifest.json has unexpected cudaVersion '$($Manifest.cudaVersion)'"
    }
    if ([string]$Manifest.cudaRuntimeVersion -ne '13.4.49') {
        throw "runtime-manifest.json has unexpected cudaRuntimeVersion '$($Manifest.cudaRuntimeVersion)'"
    }
    if ([string]$Manifest.tensorRtVersion -ne '11.3.0.99') {
        throw "runtime-manifest.json has unexpected tensorRtVersion '$($Manifest.tensorRtVersion)'"
    }
    if ([int]$Manifest.runtimeAbi -ne 2) {
        throw "runtime-manifest.json has unsupported runtimeAbi '$($Manifest.runtimeAbi)'"
    }

    $actualArchitectures = @($Manifest.architectures)
    if ($actualArchitectures.Count -ne $ExpectedArchitectures.Count) {
        throw "runtime-manifest.json must declare exactly $($ExpectedArchitectures.Count) architectures; found $($actualArchitectures.Count)"
    }

    foreach ($expected in $ExpectedArchitectures) {
        $matches = @($actualArchitectures | Where-Object { [string]$_.key -eq $expected.Key })
        if ($matches.Count -ne 1) {
            throw "runtime-manifest.json must declare architecture '$($expected.Key)' exactly once"
        }

        $actual = $matches[0]
        if ([string]$actual.computeCapability -ne $expected.ComputeCapability) {
            throw "runtime-manifest.json architecture '$($expected.Key)' has computeCapability '$($actual.computeCapability)'"
        }
        if ([string]$actual.builderResource -ne $expected.BuilderResource) {
            throw "runtime-manifest.json architecture '$($expected.Key)' has builderResource '$($actual.builderResource)'"
        }
    }
}

function Read-ChecksumList {
    param([Parameter(Mandatory = $true)][string]$Path)

    $checksums = @{}
    foreach ($line in @(Get-Content -LiteralPath $Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -notmatch '^([0-9A-Fa-f]{64})\s+\*?(.+)$') {
            throw "Invalid SHA256SUMS.txt line: '$line'"
        }

        $name = $Matches[2].Trim()
        if ($checksums.ContainsKey($name)) {
            throw "SHA256SUMS.txt contains duplicate entry '$name'"
        }
        $checksums[$name] = $Matches[1].ToLowerInvariant()
    }
    $checksums
}

try {
    $modulePath = Join-Path $PSScriptRoot 'RifeRuntimeManifest.psm1'
    Import-Module $modulePath -Force

    $root = (Resolve-Path -LiteralPath $BundleRoot).Path
    $manifestPath = Join-Path $root 'runtime-manifest.json'
    $checksumPath = Join-Path $root 'SHA256SUMS.txt'
    $archiveNames = @(Get-RifeRuntimeArchiveNames)
    $expectedArchitectures = @(Get-RifeExpectedArchitectures)

    foreach ($required in @($archiveNames + @('runtime-manifest.json', 'SHA256SUMS.txt'))) {
        $path = Join-Path $root $required
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "RIFE runtime bundle is missing required file '$required'"
        }
    }

    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert-Manifest -Manifest $manifest -ExpectedArchitectures $expectedArchitectures

    $commonEntries = @(Get-ZipFileEntries -ArchivePath (Join-Path $root 'MPCVR-RIFE-Common.zip'))
    Assert-EqualSet -Actual $commonEntries -Expected @(Get-RifeCommonRuntimeFileNames) -Description 'MPCVR-RIFE-Common.zip'

    foreach ($architecture in $expectedArchitectures) {
        $archiveName = "MPCVR-RIFE-$($architecture.Key).zip"
        $entries = @(Get-ZipFileEntries -ArchivePath (Join-Path $root $archiveName))
        Assert-EqualSet -Actual $entries -Expected @($architecture.BuilderResource) -Description $archiveName
    }

    $checksums = Read-ChecksumList -Path $checksumPath
    Assert-EqualSet -Actual @($checksums.Keys) -Expected $archiveNames -Description 'SHA256SUMS.txt'

    foreach ($archiveName in $archiveNames) {
        $actual = (Get-FileHash -LiteralPath (Join-Path $root $archiveName) -Algorithm SHA256).Hash.ToLowerInvariant()
        $expected = [string]$checksums[$archiveName]
        if ($actual -ne $expected) {
            throw "SHA-256 mismatch for '$archiveName': expected $expected, got $actual"
        }
    }

    Write-Host "RIFE runtime package validation passed: $root"
    exit 0
}
catch {
    Write-Output $_.Exception.Message
    exit 1
}
