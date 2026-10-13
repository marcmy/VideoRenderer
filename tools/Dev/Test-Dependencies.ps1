#requires -Version 5.1
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot '..\MaxineSetup\DependencyDownloads.psm1') -Force
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('mpcvr-dependency-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $fixture | Out-Null
function Check([bool]$Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
try {
    $lockPath=Join-Path $PSScriptRoot '..\..\dependencies.lock.json'
    $lock=Read-MPCVRDependencyLock -Path $lockPath
    foreach ($architecture in @('sm75','sm86','sm89','sm120')) {
        Check ([bool]$lock.components.PSObject.Properties['rife-runtime'].Value.manifest.files.PSObject.Properties["MPCVR-RIFE-$architecture.zip"]) "Missing architecture $architecture"
    }
    $bad=Get-Content $lockPath -Raw | ConvertFrom-Json
    $bad.components.PSObject.Properties['maxine'].Value.manifest.files.PSObject.Properties['MPCVR-Maxine-Runtime.zip'].Value.url='https://github.com/marcmy/VideoRenderer/releases/latest/download/runtime.zip'
    $badPath=Join-Path $fixture 'bad.json'; $bad | ConvertTo-Json -Depth 30 | Set-Content $badPath
    $rejected=$false
    try { Read-MPCVRDependencyLock -Path $badPath | Out-Null } catch { $rejected=$true }
    Check $rejected 'Moving or historical main-repository dependency URL accepted'
    $payload=Join-Path $fixture 'cached-content'; [IO.File]::WriteAllBytes($payload,[Text.Encoding]::ASCII.GetBytes('verified fixture'))
    $sha=(Get-FileHash $payload -Algorithm SHA256).Hash.ToLowerInvariant()
    $cache=Join-Path $fixture 'cache'; New-Item -ItemType Directory $cache | Out-Null
    Copy-Item $payload (Join-Path $cache $sha)
    $entry=[pscustomobject]@{sha256=$sha;bytes=(Get-Item $payload).Length;url='https://invalid.example/never-download'}
    $resolved=Get-MPCVRVerifiedDownload -Entry $entry -CacheRoot $cache
    Check ($resolved -eq (Join-Path $cache $sha)) 'Verified cache was not reused offline'
    # Exercise failure before installation; mock the HTTP downloader inside the module.
    $module=Get-Module DependencyDownloads
    & $module { function script:Invoke-WebRequest { param($Uri,$OutFile,[switch]$UseBasicParsing) [IO.File]::WriteAllText($OutFile,'corrupt download') } }
    [IO.File]::WriteAllText((Join-Path $cache $sha),'corrupt cache')
    $rejected=$false
    try { Get-MPCVRVerifiedDownload -Entry $entry -CacheRoot $cache | Out-Null } catch { $rejected=$true }
    Check $rejected 'Corrupted cache/download was accepted'
    Check (@(Get-ChildItem $cache -Filter '*.partial').Count -eq 0) 'Failed partial download remained'
    Check ([IO.File]::ReadAllText((Join-Path $cache $sha)) -eq 'corrupt cache') 'Failed download replaced the cache'
    # Resolve each GPU against tiny verified cached assets, never real runtimes.
    [IO.File]::WriteAllBytes((Join-Path $cache $sha),[IO.File]::ReadAllBytes($payload))
    $small=Get-Content $lockPath -Raw | ConvertFrom-Json
    foreach ($component in $small.components.PSObject.Properties) {
        foreach ($asset in $component.Value.manifest.files.PSObject.Properties) {
            $asset.Value.sha256=$sha; $asset.Value.bytes=(Get-Item $payload).Length
        }
    }
    foreach ($architecture in @('sm75','sm86','sm89','sm120')) {
        $destination=Join-Path $fixture $architecture
        Resolve-MPCVRDependencies -Lock $small -Destination $destination -ArchitectureKeys @($architecture) -CacheRoot $cache
        $packs=@(Get-ChildItem $destination -Filter 'MPCVR-RIFE-sm*.zip')
        Check ($packs.Count -eq 1 -and $packs[0].Name -eq "MPCVR-RIFE-$architecture.zip") 'GPU selection downloaded an unrelated architecture'
        Check (Test-Path -LiteralPath (Join-Path $destination 'PAYLOAD-SHA256SUMS.txt')) 'Resolved payload lacks its verified staging inventory'
    }
    # Exercise the real updater's download/checksum/delegation path with local
    # fixture responses and a harmless setup script. No live player or install.
    $setupRoot=Join-Path $fixture 'setup'; New-Item -ItemType Directory $setupRoot | Out-Null
    $marker=Join-Path $fixture 'delegated.txt'
    @'
param([switch]$NoPause)
[IO.File]::WriteAllText($env:MPCVR_UPDATER_FIXTURE_MARKER,'verified setup invoked')
exit 0
'@ | Set-Content -LiteralPath (Join-Path $setupRoot 'Install-MPCVR-Maxine-RIFE.ps1') -Encoding UTF8
    $setupZip=Join-Path $fixture 'MPCVR-Setup.zip'
    Compress-Archive -LiteralPath (Join-Path $setupRoot 'Install-MPCVR-Maxine-RIFE.ps1') -DestinationPath $setupZip
    $setupSha=(Get-FileHash $setupZip -Algorithm SHA256).Hash.ToLowerInvariant()
    "$setupSha  MPCVR-Setup.zip" | Set-Content -LiteralPath (Join-Path $fixture 'SHA256SUMS.txt') -Encoding ASCII
    $routingFixture=Join-Path $fixture 'updater-routing.ps1'
    @'
param([string]$Root,[string]$Updater)
$ErrorActionPreference='Stop'
$env:MPCVR_UPDATER_FIXTURE_MARKER=Join-Path $Root 'delegated.txt'
function Get-Process { param($Name,$ErrorAction) return $null }
function Test-Path {
    [CmdletBinding(DefaultParameterSetName='Path')]
    param([Parameter(ParameterSetName='Path',Position=0)][string[]]$Path,
          [Parameter(ParameterSetName='LiteralPath')][Alias('PSPath')][string[]]$LiteralPath,
          [Microsoft.PowerShell.Commands.TestPathType]$PathType='Any')
    if ($LiteralPath -like 'C:\Program Files (x86)\K-Lite Codec Pack*') { return $true }
    return Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
function Invoke-WebRequest {
    param($Uri,$OutFile,[switch]$UseBasicParsing)
    if ($Uri -eq 'https://api.github.com/repos/marcmy/VideoRenderer/releases/latest') { return [pscustomobject]@{Content='{"tag_name":"v0.10.9.2605-maxine-rife.2851"}'} }
    if ($Uri -notlike 'https://github.com/marcmy/VideoRenderer/releases/download/v0.10.9.2605-maxine-rife.2851/*') { throw 'Updater used an unpinned asset URL.' }
    $name=([Uri]$Uri).Segments[-1]
    if ($name -notin @('MPCVR-Setup.zip','SHA256SUMS.txt')) { throw 'Unexpected update asset.' }
    Copy-Item -LiteralPath (Join-Path $Root $name) -Destination $OutFile
}
& $Updater -NoPause
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $routingFixture -Encoding UTF8
    $hostExecutable=if ($PSVersionTable.PSEdition -eq 'Core') {(Get-Command pwsh.exe).Source} else {Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'}
    $updaterSource=Join-Path $PSScriptRoot '..\KLiteMaxineUpdater\Update-KLiteMPCVR.ps1'
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile($updaterSource,[ref]$tokens,[ref]$errors)
    $adminFunction=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-IsAdministrator'},$true)
    Check ([bool]$adminFunction) 'Updater fixture could not isolate its elevation check'
    $source=Get-Content -LiteralPath $updaterSource -Raw
    $instrumented=$source.Substring(0,$adminFunction.Extent.StartOffset)+'function Test-IsAdministrator { return $true }'+$source.Substring($adminFunction.Extent.EndOffset)
    $updater=Join-Path $fixture 'Update-KLiteMPCVR.ps1'
    $instrumented | Set-Content -LiteralPath $updater -Encoding UTF8
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\KLiteMaxineUpdater\KLiteRendererInstall.psm1') -Destination $fixture
    & $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File $routingFixture -Root $fixture -Updater $updater
    Check ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $marker)) "Updater did not delegate a verified, pinned setup (host $hostExecutable, exit $LASTEXITCODE)"
    Remove-Item -LiteralPath $marker -Force
    ('0'*64+'  MPCVR-Setup.zip') | Set-Content -LiteralPath (Join-Path $fixture 'SHA256SUMS.txt') -Encoding ASCII
    $previousNativePreference=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        $rejectionOutput=@(& $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File $routingFixture -Root $fixture -Updater $updater 2>&1)
        $rejectionExit=$LASTEXITCODE
    } finally { $ErrorActionPreference=$previousNativePreference }
    $rejectionText=($rejectionOutput | ForEach-Object { $_.ToString() }) -join "`n"
    if ($rejectionExit -eq 0 -or (Test-Path -LiteralPath $marker) -or $rejectionText -notlike '*SHA-256 verification failed*') {
        $rejectionOutput | ForEach-Object { Write-Host $_ }
        throw 'Updater did not reject a setup with a bad checksum before execution'
    }
    'PASS: updater rejected the intentionally invalid SHA-256; no setup executed.'
    # Exercise PATH repair without invoking the build, GPU, or developer shell.
    $devSource=Join-Path $PSScriptRoot '..\..\dev.ps1'
    $devAst=[Management.Automation.Language.Parser]::ParseFile($devSource,[ref]$tokens,[ref]$errors)
    $pathFunction=$devAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Set-BuildSearchPath'},$true)
    Check ([bool]$pathFunction) 'Build PATH repair helper is missing'
    . ([scriptblock]::Create($pathFunction.Extent.Text))
    $savedPath=$env:PATH
    try {
        $assemblerDirectory=Join-Path $fixture 'assembler'; New-Item -ItemType Directory $assemblerDirectory | Out-Null
        $assembler=Join-Path $assemblerDirectory 'nasm.exe'; [IO.File]::WriteAllText($assembler,'fixture')
        $env:PATH='C:\fixture-user-tools'
        Set-BuildSearchPath $assembler
        $systemDirectory=Join-Path $env:SystemRoot 'System32'
        Check ((Get-Command where.exe -ErrorAction Stop).Source -ieq (Join-Path $systemDirectory 'where.exe')) 'Build PATH did not restore where.exe'
        Check ((Get-Command findstr.exe -ErrorAction Stop).Source -ieq (Join-Path $systemDirectory 'findstr.exe')) 'Build PATH did not restore findstr.exe'
        Check ((Get-Command nasm.exe -ErrorAction Stop).Source -ieq $assembler) 'Build PATH did not restore pinned NASM'
        Check ($env:PATH -like '*C:\fixture-user-tools*') 'Build PATH discarded caller tools'
        $repaired=$env:PATH
        Set-BuildSearchPath $assembler
        Check ($env:PATH -eq $repaired) 'Repeated build PATH preparation accumulated duplicate entries'
        $env:PATH='C:\fixture-developer-tools'
        Set-BuildSearchPath $assembler
        Check ((Get-Command nasm.exe -ErrorAction Stop).Source -ieq $assembler -and $env:PATH -like '*C:\fixture-developer-tools*') 'Build PATH repair failed after developer-shell replacement'
    } finally { $env:PATH=$savedPath }
    'PASS: build PATH restores Windows utilities and pinned NASM before/after developer-shell replacement.'
    'Dependency contract passed: four architecture selections, exact URLs, offline cache reuse and failure isolation.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    if ($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf) -like 'mpcvr-dependency-test-*') { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
