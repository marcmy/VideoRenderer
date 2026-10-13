#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$RepoRoot = (Join-Path $PSScriptRoot '..\..'),
    [ValidateSet('auto','release','local','test')][string]$Flavor = 'auto',
    [switch]$Generate
)
$ErrorActionPreference = 'Stop'
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
function Read-Git([string[]]$Arguments) {
    $result = & git -C $RepoRoot -c "safe.directory=$RepoRoot" @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed ($LASTEXITCODE)" }
    return ($result -join "`n").TrimEnd()
}
function Hash-Bytes([byte[]]$Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}
$header = Get-Content -LiteralPath (Join-Path $RepoRoot 'Include/Version.h') -Raw
function Define-Value([string]$Name) {
    $match = [regex]::Match($header, "(?m)^#define\s+$Name\s+(\S+)")
    if (-not $match.Success) { throw "Missing version definition $Name" }
    return $match.Groups[1].Value
}
$commit = Read-Git @('rev-parse','HEAD')
$branch = Read-Git @('rev-parse','--abbrev-ref','HEAD')
$base = Define-Value 'VER_REVISION_BASE'
$baseCommit = Define-Value 'VER_REVISION_BASE_COMMIT'
& git -C $RepoRoot -c "safe.directory=$RepoRoot" merge-base --is-ancestor $baseCommit HEAD
if ($LASTEXITCODE -ne 0) { throw 'Fork revision base is not an ancestor; fetch full history before building.' }
$revision = [int]$base + [int](Read-Git @('rev-list','--first-parent','--count',"${baseCommit}..HEAD"))
$patch = Read-Git @('diff','HEAD','--binary','--')
$untracked = @((Read-Git @('ls-files','--others','--exclude-standard')) -split "`n" | Where-Object { $_ })
$dirty = -not [string]::IsNullOrEmpty($patch) -or $untracked.Count -gt 0
$identity = New-Object Text.StringBuilder
[void]$identity.Append($commit).Append("`n").Append($patch)
foreach ($relative in ($untracked | Sort-Object)) {
    $path = Join-Path $RepoRoot $relative
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        [void]$identity.Append("`n").Append($relative).Append(':').Append((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant())
    }
}
$sourceDigest = Hash-Bytes ([Text.Encoding]::UTF8.GetBytes($identity.ToString()))
if ($Flavor -eq 'auto' -and $env:MPCVR_BUILD_FLAVOR) { $Flavor = $env:MPCVR_BUILD_FLAVOR }
if ($Flavor -eq 'auto') {
    $Flavor = if ($env:MPCVR_TEST_REVISION) { 'test' } elseif ($dirty -or ($branch -ne 'master' -and $env:GITHUB_REF -ne 'refs/heads/master')) { 'local' } else { 'release' }
}
if ($Flavor -notin @('release','local','test')) { throw "Unsupported build flavor $Flavor" }
if ($env:MPCVR_TEST_REVISION) {
    if ($Flavor -eq 'release') { throw 'MPCVR_TEST_REVISION cannot label a public release.' }
    $override = 0
    if (-not [int]::TryParse($env:MPCVR_TEST_REVISION,[ref]$override)) { throw 'MPCVR_TEST_REVISION must be numeric.' }
    $revision = $override
}
if ($revision -lt 0 -or $revision -gt 65535) { throw 'Renderer revision is outside the Windows version-field range.' }
if ($Flavor -eq 'release' -and $dirty) { throw 'Public release builds require clean, committed source.' }
$version = '{0}.{1}.{2}.{3}' -f (Define-Value 'VER_MAJOR'),(Define-Value 'VER_MINOR'),(Define-Value 'VER_BUILD'),$revision
$sourceId = $commit.Substring(0,12)
if ($dirty) { $sourceId += '-dirty.' + $sourceDigest.Substring(0,16) }
$display = if ($Flavor -eq 'release') { $version } else { "$version+$Flavor.$sourceId" }
$date = Read-Git @('show','-s','--format=%cs','HEAD')
$result = [ordered]@{schemaVersion=1;version=$version;productVersion=$display;revision=$revision;flavor=$Flavor;commit=$commit;branch=$branch;sourceId=$sourceId;sourceDigest=$sourceDigest;dirty=$dirty;untrackedFiles=$untracked;commitDate=$date}
if ($Generate) {
    $safeBranch = $branch -replace '[^A-Za-z0-9_]','_'
    $legacyHash = $sourceId -replace '[^A-Za-z0-9_]','_'
    $localFlag = if ($Flavor -eq 'release') { 0 } else { 1 }
    $lines = @('#pragma once',"#define REV_DATE $($date.Replace('-','.'))", "#define REV_HASH $legacyHash", "#define REV_NUM $revision", "#define REV_BRANCH $safeBranch", "#define REV_SOURCE_ID `"$sourceId`"", "#define REV_COMMIT `"$commit`"", "#define REV_PRODUCT_VERSION `"$display`"", "#define REV_IS_LOCAL $localFlag")
    $text = ($lines -join "`n") + "`n"
    $target = Join-Path $RepoRoot 'revision.h'
    if (-not (Test-Path -LiteralPath $target) -or [IO.File]::ReadAllText($target) -ne $text) { [IO.File]::WriteAllText($target,$text,[Text.Encoding]::ASCII) }
    $bin = Join-Path $RepoRoot '_bin'
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    [IO.File]::WriteAllText((Join-Path $bin 'source-version.json'),($result | ConvertTo-Json -Depth 5) + "`n",(New-Object Text.UTF8Encoding($false)))
}
$result | ConvertTo-Json -Depth 5
