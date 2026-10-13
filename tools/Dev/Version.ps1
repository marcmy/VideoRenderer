#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$UpstreamRef = 'upstream/master',
    [string]$RecordUpstream,
    [ValidateSet('auto','release','local','test')][string]$Flavor = 'auto',
    [switch]$Generate
)
$ErrorActionPreference = 'Stop'
if (-not $RepoRoot) { $RepoRoot = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..' }
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
function Read-Git([string[]]$Arguments) {
    $result = & git -C $RepoRoot -c "safe.directory=$RepoRoot" @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed ($LASTEXITCODE)" }
    return ($result -join "`n").TrimEnd([char[]]"`r`n")
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
# Upstream counts its complete reachable history, including merged branches.
# Use the actual merged upstream ancestor, not a release note or fork history.
if ((Read-Git @('rev-parse','--is-shallow-repository')) -eq 'true') { throw 'Versioning requires full Git history; fetch --unshallow first.' }
$upstreamPinPath=Join-Path $RepoRoot 'upstream.version.json'
if ($RecordUpstream) {
    $recorded=Read-Git @('rev-parse',"$RecordUpstream^{commit}")
    $pin=[ordered]@{schemaVersion=1;repository='Aleksoid1978/VideoRenderer';commit=$recorded}
    [IO.File]::WriteAllText($upstreamPinPath,(($pin | ConvertTo-Json).Replace("`r`n","`n"))+"`n",(New-Object Text.UTF8Encoding($false)))
    return
}
if ((Test-Path -LiteralPath $upstreamPinPath) -and -not $PSBoundParameters.ContainsKey('UpstreamRef')) {
    $pin=Get-Content -LiteralPath $upstreamPinPath -Raw | ConvertFrom-Json
    if ($pin.schemaVersion -ne 1 -or $pin.repository -ne 'Aleksoid1978/VideoRenderer' -or $pin.commit -notmatch '^[a-f0-9]{40}$') { throw 'Invalid committed upstream source pin.' }
    $upstreamCommit=$pin.commit
    & git -C $RepoRoot merge-base --is-ancestor $upstreamCommit HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Pinned upstream source is not merged into this build.' }
    # A moved unrelated remote ref never relabels an existing source revision.
    & git -C $RepoRoot show-ref --verify --quiet refs/remotes/upstream/master
    if ($LASTEXITCODE -eq 0) {
        $candidate=Read-Git @('merge-base','HEAD','upstream/master')
        & git -C $RepoRoot merge-base --is-ancestor $upstreamCommit $candidate
        if ($LASTEXITCODE -eq 0 -and $candidate -ne $upstreamCommit) { throw 'New upstream source is merged but not recorded. Update upstream.version.json in the integration commit.' }
    }
} else { $upstreamCommit = Read-Git @('merge-base','HEAD',$UpstreamRef) }
$upstreamRevision = [int](Read-Git @('rev-list','--count',$upstreamCommit))
$upstreamHeader = Read-Git @('show',"${upstreamCommit}:Include/Version.h")
$upstreamParts = foreach ($name in @('VER_MAJOR','VER_MINOR','VER_BUILD')) {
    $match = [regex]::Match($upstreamHeader,"(?m)^#define\s+$name\s+(\d+)")
    if (-not $match.Success) { throw "Merged upstream is missing $name" }
    $match.Groups[1].Value
}
$upstreamVersion = ($upstreamParts -join '.') + '.' + $upstreamRevision
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
$releaseName = "MPC Video Renderer $upstreamVersion + Maxine/RIFE r$revision"
$tag = "v$upstreamVersion-maxine-rife.$revision"
$display = "$upstreamVersion + Maxine/RIFE r$revision"
if ($Flavor -ne 'release') { $display += " ($Flavor.$sourceId)" }
$date = Read-Git @('show','-s','--format=%cs','HEAD')
$result = [ordered]@{schemaVersion=2;version=$version;productVersion=$display;releaseName=$releaseName;releaseTag=$tag;revision=$revision;upstreamVersion=$upstreamVersion;upstreamRevision=$upstreamRevision;upstreamCommit=$upstreamCommit;flavor=$Flavor;commit=$commit;branch=$branch;sourceId=$sourceId;sourceDigest=$sourceDigest;dirty=$dirty;untrackedFiles=$untracked;commitDate=$date}
if ($Generate) {
    $safeBranch = $branch -replace '[^A-Za-z0-9_]','_'
    $legacyHash = $sourceId -replace '[^A-Za-z0-9_]','_'
    $localFlag = if ($Flavor -eq 'release') { 0 } else { 1 }
    $lines = @('#pragma once',"#define REV_DATE $($date.Replace('-','.'))", "#define REV_HASH $legacyHash", "#define REV_NUM $revision", "#define REV_BRANCH $safeBranch", "#define REV_SOURCE_ID `"$sourceId`"", "#define REV_COMMIT `"$commit`"", "#define REV_UPSTREAM_VERSION `"$upstreamVersion`"", "#define REV_UPSTREAM_COMMIT `"$upstreamCommit`"", "#define REV_PRODUCT_VERSION `"$display`"", "#define REV_IS_LOCAL $localFlag")
    $text = ($lines -join "`n") + "`n"
    $target = Join-Path $RepoRoot 'revision.h'
    if (-not (Test-Path -LiteralPath $target) -or [IO.File]::ReadAllText($target) -ne $text) { [IO.File]::WriteAllText($target,$text,[Text.Encoding]::ASCII) }
    $bin = Join-Path $RepoRoot '_bin'
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    [IO.File]::WriteAllText((Join-Path $bin 'source-version.json'),($result | ConvertTo-Json -Depth 5) + "`n",(New-Object Text.UTF8Encoding($false)))
}
$result | ConvertTo-Json -Depth 5
