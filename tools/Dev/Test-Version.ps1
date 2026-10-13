#requires -Version 5.1
$ErrorActionPreference='Stop'
$script=Join-Path $PSScriptRoot 'Version.ps1'
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('mpcvr-version-test-'+[guid]::NewGuid().ToString('N'))
$savedRevision=$env:MPCVR_TEST_REVISION
New-Item -ItemType Directory -Path (Join-Path $fixture 'Include') -Force | Out-Null
function Invoke-FixtureGit([string[]]$Arguments) {
    & git -C $fixture -c "safe.directory=$fixture" @Arguments | Out-Null
    if($LASTEXITCODE -ne 0){throw "Fixture git failed: $Arguments"}
}
function Check([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
try {
    Invoke-FixtureGit @('init','-b','master')
    Invoke-FixtureGit @('config','user.name','Version fixture')
    Invoke-FixtureGit @('config','user.email','fixture@example.invalid')
    Set-Content -LiteralPath (Join-Path $fixture '.gitignore') -Value "revision.h`n_bin/"
    Set-Content -LiteralPath (Join-Path $fixture 'source.cpp') -Value 'int source=1;'
    Invoke-FixtureGit @('add','.')
    Invoke-FixtureGit @('commit','-m','base')
    $base=(& git -C $fixture rev-parse HEAD).Trim()
    $header="#define VER_MAJOR 0`n#define VER_MINOR 10`n#define VER_BUILD 9`n#define VER_REVISION_BASE 2844`n#define VER_REVISION_BASE_COMMIT $base`n"
    Set-Content -LiteralPath (Join-Path $fixture 'Include/Version.h') -Value $header
    Invoke-FixtureGit @('add','.')
    Invoke-FixtureGit @('commit','-m','version contract')
    $env:MPCVR_TEST_REVISION=$null
    $clean=& $script -RepoRoot $fixture -Flavor release -Generate | ConvertFrom-Json
    Check ($clean.version -eq '0.10.9.2845' -and $clean.productVersion -eq $clean.version -and -not $clean.dirty) 'Clean release version mismatch'
    $stamp=(Get-Item -LiteralPath (Join-Path $fixture 'revision.h')).LastWriteTimeUtc
    & $script -RepoRoot $fixture -Flavor release -Generate | Out-Null
    Check ((Get-Item -LiteralPath (Join-Path $fixture 'revision.h')).LastWriteTimeUtc -eq $stamp) 'Unchanged header must not cause rebuild'
    Set-Content -LiteralPath (Join-Path $fixture 'source.cpp') -Value 'int source=2;'
    $dirty=& $script -RepoRoot $fixture -Flavor local | ConvertFrom-Json
    Check ($dirty.dirty -and $dirty.productVersion -ne $clean.productVersion) 'Dirty build identity missing'
    Set-Content -LiteralPath (Join-Path $fixture 'experiment.cpp') -Value 'int experiment=1;'
    $extra1=& $script -RepoRoot $fixture -Flavor local | ConvertFrom-Json
    Set-Content -LiteralPath (Join-Path $fixture 'experiment.cpp') -Value 'int experiment=2;'
    $extra2=& $script -RepoRoot $fixture -Flavor local | ConvertFrom-Json
    Check ($extra1.sourceDigest -ne $extra2.sourceDigest -and $extra2.untrackedFiles.Count -eq 1) 'Untracked source must affect identity'
    $rejected=$false
    try { & $script -RepoRoot $fixture -Flavor release | Out-Null } catch { $rejected=$true }
    Check $rejected 'Dirty public release was accepted'
    $env:MPCVR_TEST_REVISION='2850'
    $test=& $script -RepoRoot $fixture -Flavor test | ConvertFrom-Json
    Check ($test.revision -eq 2850 -and $test.productVersion -like '*+test.*dirty*') 'Experimental revision identity missing'
    $rejected=$false
    try { & $script -RepoRoot $fixture -Flavor release | Out-Null } catch { $rejected=$true }
    Check $rejected 'Test revision must not label a release'
    $env:MPCVR_TEST_REVISION='not-a-number'
    $rejected=$false
    try { & $script -RepoRoot $fixture -Flavor test | Out-Null } catch { $rejected=$true }
    Check $rejected 'Invalid override was accepted'
    'Version contract passed: clean/dirty/untracked/override identity and incremental header checks'
} finally {
    $env:MPCVR_TEST_REVISION=$savedRevision
    # Only this test's freshly created temporary fixture can be removed.
    $resolved=[IO.Path]::GetFullPath($fixture)
    if($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf) -like 'mpcvr-version-test-*') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
