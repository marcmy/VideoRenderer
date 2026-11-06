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
    $selection=Join-Path $fixture 'maxine-only'
    Resolve-MPCVRDependencies -Lock $small -Destination $selection -ArchitectureKeys @('sm75') -CacheRoot $cache -Components @('maxine')
    Check ((Test-Path (Join-Path $selection 'MPCVR-Maxine-Runtime.zip')) -and -not (Test-Path (Join-Path $selection 'MPCVR-RIFE-Models.zip')) -and -not (Test-Path (Join-Path $selection 'MPCVR-RIFE-Common.zip'))) 'Resolving Maxine also fetched current RIFE components'
    $selection=Join-Path $fixture 'none-needed'
    Resolve-MPCVRDependencies -Lock $small -Destination $selection -ArchitectureKeys @('sm75') -CacheRoot (Join-Path $fixture 'empty-cache') -Components @()
    Check (@(Get-ChildItem $selection -Filter '*.zip').Count -eq 0) 'Resolving no components fetched an archive'
    # Repeated setup must leave intact dependencies and engine caches alone.
    # Tiny ZIPs exercise the real archive-to-install mapping without loading DLLs.
    $reuseLock=Get-Content $lockPath -Raw | ConvertFrom-Json
    $reuseCache=Join-Path $fixture 'reuse-cache'; New-Item -ItemType Directory $reuseCache | Out-Null
    function New-ReuseArchive($Component,$Name,$Files) {
        $source=Join-Path $fixture ([guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory $source | Out-Null
        foreach ($file in $Files.GetEnumerator()) {
            $target=Join-Path $source $file.Key
            New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
            [IO.File]::WriteAllText($target,$file.Value)
        }
        $zip=$source+'.zip'; Compress-Archive -Path (Join-Path $source '*') -DestinationPath $zip
        $hash=(Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
        Copy-Item $zip (Join-Path $reuseCache $hash)
        $asset=$reuseLock.components.PSObject.Properties[$Component].Value.manifest.files.PSObject.Properties[$Name].Value
        $asset.sha256=$hash; $asset.bytes=(Get-Item $zip).Length
    }
    $maxineFiles=@{}
    foreach ($name in @('NVCVImage.dll','NVVideoEffects.dll','nvngxruntime.dll','nvngx_vsr.dll','nvVFXVideoSuperRes.dll')) { $maxineFiles["sdk/bin/$name"]="fixture $name" }
    $maxineFiles['sdk/bin/models/vsr.bin']='fixture Maxine model'
    New-ReuseArchive 'maxine' 'MPCVR-Maxine-Runtime.zip' $maxineFiles
    New-ReuseArchive 'rife-runtime' 'MPCVR-RIFE-Common.zip' @{'MPCVRRifeRuntime64.dll'='runtime';'nvinfer_11.dll'='TensorRT';'LICENSES/CUDA.txt'='license'}
    New-ReuseArchive 'rife-runtime' 'MPCVR-RIFE-sm75.zip' @{'nvinfer_builder_resource_sm75_11.dll'='sm75'}
    New-ReuseArchive 'rife-runtime' 'MPCVR-RIFE-sm86.zip' @{'nvinfer_builder_resource_sm86_11.dll'='sm86'}
    New-ReuseArchive 'rife-models' 'MPCVR-RIFE-Models.zip' @{'rife_v4.6.onnx'='model';'rife.features.onnx'='encoder';'model-manifest.json'='{"schemaVersion":3,"models":[{"model":"RIFE 4.6","file":"rife_v4.6.onnx"}]}'}
    $maxineRoot=Join-Path $fixture 'Maxine'; $rifeRoot=Join-Path $fixture 'RIFE'
    Check (-not (Test-MPCVRInstalledDependency -Lock $reuseLock -Component maxine -InstallRoot $maxineRoot -ArchitectureKeys @('sm75') -CacheRoot $reuseCache)) 'Missing Maxine was considered installed'
    foreach ($file in $maxineFiles.GetEnumerator()) {
        $target=Join-Path $maxineRoot ('nvvfx/libs/'+$file.Key.Substring('sdk/bin/'.Length))
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        [IO.File]::WriteAllText($target,$file.Value)
    }
    foreach ($file in @{'runtime/MPCVRRifeRuntime64.dll'='runtime';'runtime/nvinfer_11.dll'='TensorRT';'runtime/LICENSES/CUDA.txt'='license';'runtime/nvinfer_builder_resource_sm75_11.dll'='sm75';'models/rife_v4.6.onnx'='model';'models/rife.features.onnx'='encoder'}.GetEnumerator()) {
        $target=Join-Path $rifeRoot $file.Key
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        [IO.File]::WriteAllText($target,$file.Value)
    }
    $installedMetadata=@{
        schemaVersion=2;runtimeAbi=2;tensorRtVersion='11.4.0';tensorRtMajorMinor='11.4';cudaVersion='13.4';cudaRuntimeVersion='13.4'
        commonRuntimeArchiveSha256=$reuseLock.components.PSObject.Properties['rife-runtime'].Value.manifest.files.PSObject.Properties['MPCVR-RIFE-Common.zip'].Value.sha256
        architectures=@(@{key='sm75';archive='MPCVR-RIFE-sm75.zip';sha256=$reuseLock.components.PSObject.Properties['rife-runtime'].Value.manifest.files.PSObject.Properties['MPCVR-RIFE-sm75.zip'].Value.sha256;builderResource='nvinfer_builder_resource_sm75_11.dll'})
        models=@(@{model='RIFE 4.6';modelFile='models/rife_v4.6.onnx';modelSha256=(Get-FileHash (Join-Path $rifeRoot 'models/rife_v4.6.onnx') -Algorithm SHA256).Hash.ToLowerInvariant()})
    }
    $installedMetadataPath=Join-Path $rifeRoot 'installed-manifest.json'
    $installedMetadata | ConvertTo-Json -Depth 8 | Set-Content $installedMetadataPath
    $reuseArgs=@{Lock=$reuseLock;ArchitectureKeys=@('sm75');CacheRoot=$reuseCache}
    Check (Test-MPCVRInstalledDependency @reuseArgs -Component maxine -InstallRoot $maxineRoot) 'Existing Maxine was not adopted from the pinned cached archive'
    Check (Test-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot) 'Existing RIFE was not adopted from the pinned cached archives'
    $validMetadata=[IO.File]::ReadAllText($installedMetadataPath)
    foreach ($invalid in @('invalid JSON','{"schemaVersion":2}',($validMetadata.Replace('"runtimeAbi":  2','"runtimeAbi":  1').Replace('"runtimeAbi": 2','"runtimeAbi": 1')))) {
        [IO.File]::WriteAllText($installedMetadataPath,$invalid)
        Check (-not (Test-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot)) 'Invalid legacy RIFE metadata was adopted instead of repaired'
    }
    foreach ($field in @('architectures','models','commonRuntimeArchiveSha256')) {
        $invalid=$validMetadata | ConvertFrom-Json
        switch ($field) {
            'architectures' { $invalid.architectures[0].builderResource='wrong.dll' }
            'models' { $invalid.models[0].modelSha256='0'*64 }
            'commonRuntimeArchiveSha256' { $invalid.commonRuntimeArchiveSha256='0'*64 }
        }
        $invalid | ConvertTo-Json -Depth 8 | Set-Content $installedMetadataPath
        Check (-not (Test-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot)) "Invalid legacy $field metadata was adopted"
    }
    [IO.File]::WriteAllText($installedMetadataPath,$validMetadata)
    Save-MPCVRInstalledDependency @reuseArgs -Component maxine -InstallRoot $maxineRoot
    Save-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot
    $noCache=Join-Path $fixture 'no-download-cache'
    $reuseArgs.CacheRoot=$noCache
    $engine=Join-Path $rifeRoot 'cache/engine.plan'; New-Item -ItemType Directory (Split-Path $engine) | Out-Null
    [IO.File]::WriteAllText($engine,'user engine cache')
    $before=(Get-Item (Join-Path $rifeRoot 'runtime/nvinfer_11.dll')).LastWriteTimeUtc
    Check (Test-MPCVRInstalledDependency @reuseArgs -Component maxine -InstallRoot $maxineRoot) 'Unchanged Maxine needed the download cache after recording installation'
    Check (Test-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot) 'Unchanged RIFE needed the download cache after recording installation'
    Check ((Get-Item (Join-Path $rifeRoot 'runtime/nvinfer_11.dll')).LastWriteTimeUtc -eq $before -and [IO.File]::ReadAllText($engine) -eq 'user engine cache') 'Reuse changed installed binaries or engine caches'
    [IO.File]::WriteAllText((Join-Path $rifeRoot 'models/rife_v4.6.onnx'),'wrong') # same byte count
    Check (-not (Test-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot)) 'Same-size model corruption was accepted'
    [IO.File]::WriteAllText((Join-Path $rifeRoot 'models/rife_v4.6.onnx'),'model')
    Remove-Item -LiteralPath (Join-Path $maxineRoot 'nvvfx/libs/NVCVImage.dll')
    Check (-not (Test-MPCVRInstalledDependency @reuseArgs -Component maxine -InstallRoot $maxineRoot)) 'Missing Maxine DLL was accepted'
    Check (Test-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot) 'Damaged Maxine invalidated independent RIFE state'
    $reuseArgs.ArchitectureKeys=@('sm86')
    Check (-not (Test-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot)) 'A newly selected GPU pack was considered installed'
    $reuseArgs.ArchitectureKeys=@('sm75')
    $modelsPin=$reuseLock.components.PSObject.Properties['rife-models'].Value
    $savedPin=$modelsPin.manifestSha256; $modelsPin.manifestSha256='0'*64
    Check (-not (Test-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot)) 'A changed model pin reused the old installation'
    $modelsPin.manifestSha256=$savedPin
    # Corrupt receipts fail closed, including paths outside the install root.
    $receiptPath=Join-Path $rifeRoot 'dependency-receipt.json'
    $savedReceipt=[IO.File]::ReadAllText($receiptPath)
    $receipt=$savedReceipt | ConvertFrom-Json; $receipt.files[0].path='../outside.dll'
    $receipt | ConvertTo-Json -Depth 12 | Set-Content $receiptPath
    Check (-not (Test-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot)) 'An escaping receipt path was accepted'
    [IO.File]::WriteAllText($receiptPath,'invalid JSON')
    Check (-not (Test-MPCVRInstalledDependency @reuseArgs -Component rife -InstallRoot $rifeRoot)) 'A malformed receipt was accepted'
    [IO.File]::WriteAllText($receiptPath,$savedReceipt)
    # Run the actual setup coordinator against an isolated profile. External
    # install/elevation/preflight boundaries are stubs; selection and hashing are real.
    [IO.File]::WriteAllText((Join-Path $maxineRoot 'nvvfx/libs/NVCVImage.dll'),'fixture NVCVImage.dll')
    $profile=Join-Path $fixture 'setup-profile'; New-Item -ItemType Directory $profile | Out-Null
    Copy-Item $maxineRoot (Join-Path $profile 'MPCVR Maxine Runtime') -Recurse
    New-Item -ItemType Directory (Join-Path $profile 'MPCVideoRenderer') | Out-Null
    Copy-Item $rifeRoot (Join-Path $profile 'MPCVideoRenderer/RIFE') -Recurse
    $profileCache=[IO.Path]::GetFullPath((Join-Path $profile 'MPCVideoRenderer/RIFE/cache'))
    if (-not $profileCache.StartsWith([IO.Path]::GetFullPath($fixture)+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cache path escapes test root' }
    Remove-Item -LiteralPath $profileCache -Recurse -Force
    $coordinator=Join-Path $fixture 'coordinator'; $coordinatorPayload=Join-Path $coordinator 'payload'
    New-Item -ItemType Directory $coordinatorPayload -Force | Out-Null
    foreach ($name in @('Install-MPCVRMaxineRuntime.ps1','Install-MPCVRRifeRuntime.ps1','Test-MPCVRRifePreflight.ps1','Install-KLiteMPCVRUpdater.ps1','Update-KLiteMPCVR.ps1','KLiteRendererInstall.psm1','MpcVideoRenderer-Maxine-RIFE.zip')) {
        [IO.File]::WriteAllText((Join-Path $coordinatorPayload $name),'unused boundary fixture')
    }
    Copy-Item (Join-Path $PSScriptRoot '../MaxineSetup/DependencyDownloads.psm1') $coordinatorPayload
    Copy-Item (Join-Path $PSScriptRoot '../RifeRuntimeInstaller/RifeRuntimeManifest.psm1') $coordinatorPayload
    $reuseLock | ConvertTo-Json -Depth 40 | Set-Content (Join-Path $coordinatorPayload 'dependencies.lock.json')
    Get-ChildItem $coordinatorPayload -File | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$_.Name
    } | Set-Content (Join-Path $coordinatorPayload 'PAYLOAD-SHA256SUMS.txt') -Encoding ASCII
    $coordinatorSource=Get-Content (Join-Path $PSScriptRoot '../MaxineSetup/Install-MPCVR-Maxine-RIFE.ps1') -Raw
    $tokens=$null; $errors=$null
    $coordinatorAst=[Management.Automation.Language.Parser]::ParseInput($coordinatorSource,[ref]$tokens,[ref]$errors)
    $boundaries=@{
        'Invoke-SetupStep'=@'
function Invoke-SetupStep {
    param($Name,$ScriptPath,$Parameters)
    if ($Name -notmatch '^(3/4 |4/4 |Running RIFE post-install preflight)') { throw "Unexpected runtime reinstall: $Name" }
    Add-Content -LiteralPath $env:MPCVR_SETUP_TEST_STEPS -Value $Name
}
'@
        'Assert-MaxinePreflight'='function Assert-MaxinePreflight { return (Join-Path $env:LOCALAPPDATA "MPCVR Maxine Runtime/nvvfx/libs") }'
    }
    $replacements=@($coordinatorAst.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -in @('Invoke-SetupStep','Assert-MaxinePreflight')},$true) | Sort-Object {$_.Extent.StartOffset} -Descending)
    foreach ($node in $replacements) {
        $coordinatorSource=$coordinatorSource.Substring(0,$node.Extent.StartOffset)+$boundaries[$node.Name]+$coordinatorSource.Substring($node.Extent.EndOffset)
    }
    $coordinatorPath=Join-Path $coordinator 'Install-MPCVR-Maxine-RIFE.ps1'
    $coordinatorSource | Set-Content $coordinatorPath -Encoding UTF8
    $runner=Join-Path $fixture 'run-coordinator.ps1'
    @'
param($Setup,$Profile,$Steps)
$env:LOCALAPPDATA=$Profile
$env:MPCVR_SETUP_TEST_STEPS=$Steps
function global:Get-Process { param($Name,$ErrorAction) return $null }
function global:Invoke-WebRequest { throw 'A current installation attempted a network download.' }
& $Setup -NoPause -GpuInventoryJson '[{"Name":"fixture GPU","ComputeCapability":"7.5"}]'
exit $LASTEXITCODE
'@ | Set-Content $runner -Encoding UTF8
    $hostExecutable=if ($PSVersionTable.PSEdition -eq 'Core') {(Get-Command pwsh.exe).Source} else {Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'}
    $stepsPath=Join-Path $fixture 'setup-steps.txt'
    & $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runner -Setup $coordinatorPath -Profile $profile -Steps $stepsPath
    Check ($LASTEXITCODE -eq 0) 'Setup did not skip current dependencies before downloading/staging them'
    $steps=@(Get-Content $stepsPath)
    Check ($steps.Count -eq 3 -and $steps[0] -like '3/4 *' -and $steps[1] -like '4/4 *' -and $steps[2] -like 'Running RIFE post-install preflight*') 'Reuse skipped renderer/updater installation or post-install checks'
    Check (Test-Path -LiteralPath $profileCache -PathType Container) 'Setup failed to repair a missing empty cache directory during RIFE reuse'
    'PASS: installed dependency reuse, cached legacy adoption, independent components, corruption and GPU/pin changes.'
    # -PruneDownloads through the real coordinator in the isolated profile. Stale
    # names are fixtures; the "keep" and "previous" files prove what must survive.
    $profileDownloads=Join-Path $profile 'MPCVideoRenderer/downloads'
    New-Item -ItemType Directory $profileDownloads -Force | Out-Null
    $hexName={ param($Seed) -join ((Get-FileHash -InputStream ([IO.MemoryStream]::new([Text.Encoding]::ASCII.GetBytes($Seed))) -Algorithm SHA256).Hash.ToLowerInvariant()) }
    $pinnedName=$reuseLock.components.PSObject.Properties['maxine'].Value.manifest.files.PSObject.Properties['MPCVR-Maxine-Runtime.zip'].Value.sha256
    $staleName=& $hexName 'stale'
    $otherFiles=@(($staleName+'.partial'),'notes.txt',(& $hexName 'upper').ToUpperInvariant(),'abc123')
    foreach ($name in @($pinnedName,$staleName)+$otherFiles) { [IO.File]::WriteAllText((Join-Path $profileDownloads $name),'fixture') }
    New-Item -ItemType Directory (Join-Path $profileDownloads (& $hexName 'folder')) | Out-Null
    $pruneRunner=Join-Path $fixture 'run-coordinator-prune.ps1'
    @'
param($Setup,$Profile,$Steps,$Answer,[switch]$NoPause)
$env:LOCALAPPDATA=$Profile
$env:MPCVR_SETUP_TEST_STEPS=$Steps
function global:Get-Process { param($Name,$ErrorAction) return $null }
function global:Invoke-WebRequest { throw 'A current installation attempted a network download.' }
function global:Read-Host { param($Prompt) if ($Prompt -like 'Type yes*') { return $Answer } return '' }
& $Setup -NoPause:$NoPause -PruneDownloads -GpuInventoryJson '[{"Name":"fixture GPU","ComputeCapability":"7.5"}]'
exit $LASTEXITCODE
'@ | Set-Content $pruneRunner -Encoding UTF8
    $pruneHost=if ($PSVersionTable.PSEdition -eq 'Core') {(Get-Command pwsh.exe).Source} else {Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'}
    function Invoke-PruneSetup($Answer,[switch]$NoPause) {
        $output=@(& $pruneHost -NoLogo -NoProfile -ExecutionPolicy Bypass -File $pruneRunner -Setup $coordinatorPath -Profile $profile -Steps $stepsPath -Answer $Answer @(if ($NoPause) {'-NoPause'}) 2>&1 | ForEach-Object { "$_" })
        if ($LASTEXITCODE -ne 0) { $output | ForEach-Object { Write-Host $_ }; throw 'Setup with -PruneDownloads failed' }
        return $output -join "`n"
    }
    $text=Invoke-PruneSetup 'yes' -NoPause
    Check ($text -like "*$staleName*" -and (Test-Path (Join-Path $profileDownloads $staleName))) '-NoPause must list superseded downloads and delete nothing'
    Check (Test-Path (Join-Path $profileDownloads 'retained-pins.json')) 'Setup did not record the downloads it uses'
    [void](Invoke-PruneSetup 'y')
    Check (Test-Path (Join-Path $profileDownloads $staleName)) 'An answer other than yes deleted a download'
    [void](Invoke-PruneSetup 'yes')
    Check (-not (Test-Path (Join-Path $profileDownloads $staleName))) 'Confirmed cleanup kept a superseded download'
    Check (Test-Path (Join-Path $profileDownloads $pinnedName)) 'Cleanup deleted a download the current lock pins'
    foreach ($name in $otherFiles+@((& $hexName 'folder'),'retained-pins.json')) {
        Check (Test-Path (Join-Path $profileDownloads $name)) "Cleanup touched something that is not a superseded download: $name"
    }
    # A new pin moves the installed set to "previous"; the set before that goes.
    $pruneCache=Join-Path $fixture 'prune-cache'; New-Item -ItemType Directory $pruneCache | Out-Null
    $pinLock=Get-Content $lockPath -Raw | ConvertFrom-Json
    $pinAsset=$pinLock.components.PSObject.Properties['rife-runtime'].Value.manifest.files.PSObject.Properties['SHA256SUMS.txt'].Value
    $generations=@('generation one','generation two','generation three' | ForEach-Object { & $hexName $_ })
    foreach ($generation in $generations) { [IO.File]::WriteAllText((Join-Path $pruneCache $generation),'fixture') }
    $superseded={ @(Get-MPCVRSupersededDownload -Lock $pinLock -CacheRoot $pruneCache | ForEach-Object Name) }
    $pinAsset.sha256=$generations[0]
    Save-MPCVRDownloadRetention -Lock $pinLock -CacheRoot $pruneCache
    Save-MPCVRDownloadRetention -Lock $pinLock -CacheRoot $pruneCache
    $pinAsset.sha256=$generations[1]
    Check (@(& $superseded).Count -eq 1 -and (& $superseded) -eq $generations[2]) 'The recorded set was not kept before setup succeeded on the new pin'
    Save-MPCVRDownloadRetention -Lock $pinLock -CacheRoot $pruneCache
    Save-MPCVRDownloadRetention -Lock $pinLock -CacheRoot $pruneCache
    Check (@(& $superseded).Count -eq 1 -and (& $superseded) -eq $generations[2]) 'Rerunning the same setup forgot the previous pin'
    $pinAsset.sha256=$generations[2]
    Save-MPCVRDownloadRetention -Lock $pinLock -CacheRoot $pruneCache
    Check (@(& $superseded).Count -eq 1 -and (& $superseded) -eq $generations[0]) 'Cleanup did not keep exactly the current and previous pins'
    $removal=Remove-MPCVRSupersededDownload -Lock $pinLock -CacheRoot $pruneCache
    Check ($removal.Count -eq 1 -and $removal.Bytes -eq 7 -and -not (Test-Path (Join-Path $pruneCache $generations[0]))) 'Cleanup did not report what it removed'
    [IO.File]::WriteAllText((Join-Path $pruneCache (& $hexName 'after damage')),'fixture')
    [IO.File]::WriteAllText((Join-Path $pruneCache 'retained-pins.json'),'invalid JSON')
    $rejected=$false
    try { Remove-MPCVRSupersededDownload -Lock $pinLock -CacheRoot $pruneCache | Out-Null } catch { $rejected=$true }
    Check ($rejected -and (Test-Path (Join-Path $pruneCache (& $hexName 'after damage')))) 'An unreadable retention record allowed deletion'
    'PASS: -PruneDownloads asks first, keeps current and previous pins, and removes only superseded verified downloads.'
    # A new runtime build must cost only its own small package. Mock the network and
    # count what it asks for; Libraries is already cached from the previous build.
    $split=Join-Path $fixture 'split'; $splitCache=Join-Path $split 'cache'
    New-Item -ItemType Directory $splitCache -Force | Out-Null
    function New-SplitArchive($Files,$Stem) {
        $source=Join-Path $split ([guid]::NewGuid().ToString('N')); New-Item -ItemType Directory $source | Out-Null
        foreach ($file in $Files.GetEnumerator()) { [IO.File]::WriteAllText((Join-Path $source $file.Key),$file.Value) }
        $zip=Join-Path $split "$Stem.zip"; Compress-Archive -Path (Join-Path $source '*') -DestinationPath $zip -Force
        Remove-Item -LiteralPath $source -Recurse -Force
        return $zip
    }
    function ConvertTo-SplitLock($Lock,$RuntimeZip,$LibrariesZip) {
        $manifest=$Lock.components.PSObject.Properties['rife-runtime'].Value.manifest
        if ($manifest.files.PSObject.Properties['MPCVR-RIFE-Common.zip']) { $manifest.files.PSObject.Properties.Remove('MPCVR-RIFE-Common.zip') }
        foreach ($pair in @(@('MPCVR-RIFE-Runtime.zip',$RuntimeZip),@('MPCVR-RIFE-Libraries.zip',$LibrariesZip))) {
            $manifest.files.PSObject.Properties.Remove($pair[0])
            $manifest.files | Add-Member -NotePropertyName $pair[0] -NotePropertyValue ([pscustomobject]@{
                sha256=(Get-FileHash $pair[1] -Algorithm SHA256).Hash.ToLowerInvariant();bytes=(Get-Item $pair[1]).Length
                url="https://github.com/marcmy/VideoRenderer-Dependencies/releases/download/$($manifest.releaseTag)/$($pair[0])"})
        }
    }
    $librariesZip=New-SplitArchive @{'nvinfer_11.dll'='TensorRT';'nvonnxparser_11.dll'='parser';'nvinfer_builder_resource_ptx_11.dll'='ptx'} 'libraries'
    $runtimeZip=New-SplitArchive @{'MPCVRRifeRuntime64.dll'='build one';'BUILD-INFO.txt'='info'} 'runtime-one'
    $nextRuntimeZip=New-SplitArchive @{'MPCVRRifeRuntime64.dll'='build two';'BUILD-INFO.txt'='info'} 'runtime-two'
    $splitLock=Get-Content $lockPath -Raw | ConvertFrom-Json
    # Everything except the runtime package is present already.
    foreach ($component in $splitLock.components.PSObject.Properties) {
        foreach ($asset in $component.Value.manifest.files.PSObject.Properties) { $asset.Value.sha256=$sha; $asset.Value.bytes=(Get-Item $payload).Length }
    }
    Copy-Item $payload (Join-Path $splitCache $sha)
    ConvertTo-SplitLock $splitLock $nextRuntimeZip $librariesZip
    $librariesSha=(Get-FileHash $librariesZip -Algorithm SHA256).Hash.ToLowerInvariant()
    Copy-Item $librariesZip (Join-Path $splitCache $librariesSha)
    $splitLockPath=Join-Path $split 'lock.json'
    $splitLock | ConvertTo-Json -Depth 40 | Set-Content $splitLockPath
    Check (@((Read-MPCVRDependencyLock -Path $splitLockPath).components.PSObject.Properties['rife-runtime'].Value.manifest.files.PSObject.Properties.Name) -contains 'MPCVR-RIFE-Libraries.zip') 'The split runtime lock was not accepted'
    foreach ($mutation in @('no-libraries','no-runtime','both-layouts')) {
        $invalid=Get-Content $splitLockPath -Raw | ConvertFrom-Json
        $files=$invalid.components.PSObject.Properties['rife-runtime'].Value.manifest.files
        switch ($mutation) {
            'no-libraries' { $files.PSObject.Properties.Remove('MPCVR-RIFE-Libraries.zip') }
            'no-runtime' { $files.PSObject.Properties.Remove('MPCVR-RIFE-Runtime.zip') }
            'both-layouts' { $files | Add-Member -NotePropertyName 'MPCVR-RIFE-Common.zip' -NotePropertyValue $files.PSObject.Properties['MPCVR-RIFE-Runtime.zip'].Value.PSObject.Copy() ; $files.PSObject.Properties['MPCVR-RIFE-Common.zip'].Value.url=$files.PSObject.Properties['MPCVR-RIFE-Common.zip'].Value.url.Replace('Runtime.zip','Common.zip') }
        }
        $invalid | ConvertTo-Json -Depth 40 | Set-Content $badPath
        $rejected=$false
        try { Read-MPCVRDependencyLock -Path $badPath | Out-Null } catch { $rejected=$true }
        Check $rejected "Incomplete runtime pin was accepted: $mutation"
    }
    & $module { param($Served,$Log) $script:FixtureServed=$Served; $script:FixtureLog=$Log
        function script:Invoke-WebRequest { param($Uri,$OutFile,[switch]$UseBasicParsing)
            Add-Content -LiteralPath $script:FixtureLog -Value $Uri
            Copy-Item -LiteralPath $script:FixtureServed -Destination $OutFile }
    } $nextRuntimeZip (Join-Path $split 'downloads.txt')
    $resolvedSplit=Join-Path $split 'resolved'
    Resolve-MPCVRDependencies -Lock $splitLock -Destination $resolvedSplit -ArchitectureKeys @('sm75') -CacheRoot $splitCache -Components @('rife-runtime')
    $requested=@(Get-Content (Join-Path $split 'downloads.txt'))
    Check ($requested.Count -eq 1 -and $requested[0] -like '*/MPCVR-RIFE-Runtime.zip') "A new runtime build downloaded more than its own package: $($requested -join ', ')"
    Check ((Get-FileHash (Join-Path $resolvedSplit 'MPCVR-RIFE-Libraries.zip') -Algorithm SHA256).Hash -ieq $librariesSha) 'Cached libraries were not staged for installation'
    Check (-not (Test-Path (Join-Path $resolvedSplit 'MPCVR-RIFE-Common.zip'))) 'Split lock staged a common package'
    # Corrupt the cached libraries: the pinned hash must force one replacement, never trust the file.
    [IO.File]::WriteAllText((Join-Path $splitCache $librariesSha),'corrupt')
    $rejected=$false
    try { Resolve-MPCVRDependencies -Lock $splitLock -Destination (Join-Path $split 'corrupt') -ArchitectureKeys @('sm75') -CacheRoot $splitCache -Components @('rife-runtime') } catch { $rejected=$true }
    Check $rejected 'A corrupted library package was accepted from the cache'
    Copy-Item $librariesZip (Join-Path $splitCache $librariesSha) -Force
    # The real installer: stage split packages, record both hashes, keep plans,
    # and roll back when post-swap checks fail. Only the native preflight is stubbed.
    $installer=Join-Path $split 'installer'; New-Item -ItemType Directory $installer | Out-Null
    Copy-Item (Join-Path $PSScriptRoot '../RifeRuntimeInstaller/Install-MPCVRRifeRuntime.ps1') $installer
    Copy-Item (Join-Path $PSScriptRoot '../RifeRuntimeInstaller/RifeRuntimeManifest.psm1') $installer
    @'
param($InstallRoot,$GpuInventoryJson,[switch]$ThrowOnFailure)
if ($env:MPCVR_TEST_FAIL_PREFLIGHT) { throw 'fixture preflight failure' }
'@ | Set-Content (Join-Path $installer 'Test-MPCVRRifePreflight.ps1') -Encoding UTF8
    $modelRoot=Join-Path $split 'models'; New-Item -ItemType Directory $modelRoot | Out-Null
    $modelEntries=foreach ($model in @(@('RIFE 4.4','rife_v4.4.onnx'),@('RIFE 4.6','rife_v4.6.onnx'),@('RIFE 4.15 Lite','rife_v4.15_lite.onnx'),@('RIFE 4.25','rife_v4.25.onnx'),@('RIFE 4.25 Lite','rife_v4.25_lite.onnx'))) {
        [IO.File]::WriteAllText((Join-Path $modelRoot $model[1]),"fixture $($model[1])")
        [pscustomobject]@{model=$model[0];file=$model[1];sourceRelease='fixture';sourceArchive='fixture.7z';sourceArchiveSha256='a'*64;sourceModel='fixture.onnx';input=@(1,11,'H','W');output=@(1,3,'H','W');precision='mixed-fp16-fp32'}
    }
    [pscustomobject]@{schemaVersion=3;models=@($modelEntries)} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $modelRoot 'model-manifest.json')
    Get-ChildItem $modelRoot -File | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$_.Name } | Set-Content (Join-Path $modelRoot 'SHA256SUMS.txt') -Encoding ASCII
    $sm75Zip=New-SplitArchive @{'nvinfer_builder_resource_sm75_11.dll'='sm75'} 'sm75'
    function New-RuntimeBundle($Name,[string[]]$Archives) {
        $bundle=Join-Path $split $Name; New-Item -ItemType Directory $bundle | Out-Null
        foreach ($archive in $Archives) { $target=$archive.Split('=',2); Copy-Item $target[1] (Join-Path $bundle $target[0]) }
        @{schemaVersion=1;runtimeAbi=2;tensorRtVersion='11.4.0.106';cudaVersion='13.4.1';cudaRuntimeVersion='13.4.49'
          architectures=@(@{computeCapability='7.5';key='sm75';builderResource='nvinfer_builder_resource_sm75_11.dll'})} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $bundle 'runtime-manifest.json')
        Get-ChildItem $bundle -Filter '*.zip' | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$_.Name } | Set-Content (Join-Path $bundle 'SHA256SUMS.txt') -Encoding ASCII
        return $bundle
    }
    $splitBundle=New-RuntimeBundle 'bundle-one' @("MPCVR-RIFE-Runtime.zip=$runtimeZip","MPCVR-RIFE-Libraries.zip=$librariesZip","MPCVR-RIFE-sm75.zip=$sm75Zip")
    $nextBundle=New-RuntimeBundle 'bundle-two' @("MPCVR-RIFE-Runtime.zip=$nextRuntimeZip","MPCVR-RIFE-Libraries.zip=$librariesZip","MPCVR-RIFE-sm75.zip=$sm75Zip")
    $commonZip=New-SplitArchive @{'MPCVRRifeRuntime64.dll'='legacy';'BUILD-INFO.txt'='info';'nvinfer_11.dll'='TensorRT';'nvonnxparser_11.dll'='parser';'nvinfer_builder_resource_ptx_11.dll'='ptx'} 'common'
    $legacyBundle=New-RuntimeBundle 'bundle-legacy' @("MPCVR-RIFE-Common.zip=$commonZip","MPCVR-RIFE-sm75.zip=$sm75Zip")
    $partialBundle=New-RuntimeBundle 'bundle-partial' @("MPCVR-RIFE-Runtime.zip=$runtimeZip","MPCVR-RIFE-sm75.zip=$sm75Zip")
    $inventoryFile=Join-Path $split 'gpu.json'
    [IO.File]::WriteAllText($inventoryFile,'[{"name":"fixture GPU","computeCapability":"7.5"}]')
    $installRoot=Join-Path $split 'installed'
    $hostExecutable=if ($PSVersionTable.PSEdition -eq 'Core') {(Get-Command pwsh.exe).Source} else {Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'}
    function Invoke-FixtureInstall($Bundle) {
        $previous=$ErrorActionPreference
        try {
            $ErrorActionPreference='Continue'
            & $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $installer 'Install-MPCVRRifeRuntime.ps1') -RuntimeBundleRoot $Bundle -ModelArchive $modelRoot -InstallRoot $installRoot -GpuInventoryJson $inventoryFile -NoPause 2>&1 | Tee-Object -Variable script:LastFixtureInstallOutput | Out-Null
            return $LASTEXITCODE
        } finally { $ErrorActionPreference=$previous }
    }
    $installedDll=Join-Path $installRoot 'runtime/MPCVRRifeRuntime64.dll'
    $firstInstall=Invoke-FixtureInstall $splitBundle
    if ($firstInstall -ne 0) { $script:LastFixtureInstallOutput | ForEach-Object { Write-Host $_ } }
    Check ($firstInstall -eq 0) 'Split runtime bundle did not install'
    foreach ($name in @('MPCVRRifeRuntime64.dll','BUILD-INFO.txt','nvinfer_11.dll','nvonnxparser_11.dll','nvinfer_builder_resource_ptx_11.dll','nvinfer_builder_resource_sm75_11.dll')) {
        Check (Test-Path -LiteralPath (Join-Path $installRoot "runtime/$name") -PathType Leaf) "Split install is missing $name"
    }
    $recorded=@((Get-Content (Join-Path $installRoot 'installed-manifest.json') -Raw | ConvertFrom-Json).sharedRuntimeArchives)
    Check ($recorded.Count -eq 2 -and @($recorded | Where-Object { $_.archive -eq 'MPCVR-RIFE-Libraries.zip' -and $_.sha256 -eq $librariesSha }).Count -eq 1) 'Split install did not record its package hashes'
    Check ((Invoke-FixtureInstall $partialBundle) -ne 0 -and [IO.File]::ReadAllText($installedDll) -eq 'build one') 'A bundle without libraries changed the installation'
    Add-Content -LiteralPath (Join-Path $nextBundle 'MPCVR-RIFE-Libraries.zip') -Value 'corruption'
    Check ((Invoke-FixtureInstall $nextBundle) -ne 0 -and [IO.File]::ReadAllText($installedDll) -eq 'build one') 'A corrupted library package changed the installation'
    Copy-Item $librariesZip (Join-Path $nextBundle 'MPCVR-RIFE-Libraries.zip') -Force
    [IO.File]::WriteAllText((Join-Path $installRoot 'cache/fixture.plan'),'user engine cache')
    $env:MPCVR_TEST_FAIL_PREFLIGHT='1'
    try { $failed=Invoke-FixtureInstall $nextBundle } finally { Remove-Item Env:MPCVR_TEST_FAIL_PREFLIGHT }
    Check ($failed -ne 0 -and [IO.File]::ReadAllText($installedDll) -eq 'build one') 'A failed post-swap check did not roll back the previous runtime'
    Check ((Invoke-FixtureInstall $nextBundle) -eq 0 -and [IO.File]::ReadAllText($installedDll) -eq 'build two') 'A new runtime build did not replace the installed DLL'
    Check ([IO.File]::ReadAllText((Join-Path $installRoot 'cache/fixture.plan')) -eq 'user engine cache') 'Runtime upgrade lost compatible engine plans'
    # Reuse is recorded against the pinned packages and refuses corrupted or changed ones.
    $sm75Sha=(Get-FileHash $sm75Zip -Algorithm SHA256).Hash.ToLowerInvariant()
    $asset=$splitLock.components.PSObject.Properties['rife-runtime'].Value.manifest.files.PSObject.Properties['MPCVR-RIFE-sm75.zip'].Value
    $asset.sha256=$sm75Sha; $asset.bytes=(Get-Item $sm75Zip).Length
    Copy-Item $sm75Zip (Join-Path $splitCache $sm75Sha)
    Copy-Item $nextRuntimeZip (Join-Path $splitCache (Get-FileHash $nextRuntimeZip -Algorithm SHA256).Hash.ToLowerInvariant()) -Force
    $modelZip=Join-Path $split 'models.zip'; Compress-Archive -Path (Join-Path $modelRoot '*') -DestinationPath $modelZip
    $modelSha=(Get-FileHash $modelZip -Algorithm SHA256).Hash.ToLowerInvariant()
    Copy-Item $modelZip (Join-Path $splitCache $modelSha)
    $asset=$splitLock.components.PSObject.Properties['rife-models'].Value.manifest.files.PSObject.Properties['MPCVR-RIFE-Models.zip'].Value
    $asset.sha256=$modelSha; $asset.bytes=(Get-Item $modelZip).Length
    $splitArgs=@{Lock=$splitLock;Component='rife';InstallRoot=$installRoot;ArchitectureKeys=@('sm75');CacheRoot=$splitCache}
    Check (Test-MPCVRInstalledDependency @splitArgs) 'A split install was not recognised from its pinned packages'
    Save-MPCVRInstalledDependency @splitArgs
    $splitArgs.CacheRoot=Join-Path $split 'no-cache'
    Check (Test-MPCVRInstalledDependency @splitArgs) 'A recorded split install needed the download cache'
    [IO.File]::WriteAllText((Join-Path $installRoot 'runtime/nvinfer_11.dll'),'CORRUPTED')
    Check (-not (Test-MPCVRInstalledDependency @splitArgs)) 'Corrupted installed libraries were accepted'
    [IO.File]::WriteAllText((Join-Path $installRoot 'runtime/nvinfer_11.dll'),'TensorRT')
    Check (Test-MPCVRInstalledDependency @splitArgs) 'Restored libraries were not accepted'
    $runtimePin=$splitLock.components.PSObject.Properties['rife-runtime'].Value
    $savedManifestSha=$runtimePin.manifestSha256; $runtimePin.manifestSha256='0'*64
    Check (-not (Test-MPCVRInstalledDependency @splitArgs)) 'A new runtime pin reused the previous installation'
    $runtimePin.manifestSha256=$savedManifestSha
    # An install from the common package migrates through one normal installation,
    # and the common package itself still installs for older pins.
    Remove-Item -LiteralPath (Join-Path $installRoot 'dependency-receipt.json')
    $splitArgs.CacheRoot=$splitCache
    Check ((Invoke-FixtureInstall $legacyBundle) -eq 0 -and [IO.File]::ReadAllText($installedDll) -eq 'legacy') 'The legacy common package no longer installs'
    Check (-not (Test-MPCVRInstalledDependency @splitArgs)) 'A common-package install was treated as the pinned split packages'
    Check ((Invoke-FixtureInstall $nextBundle) -eq 0 -and (Test-MPCVRInstalledDependency @splitArgs)) 'A common-package install did not migrate to the split packages'
    # The setup coordinator builds the installer's bundle from the staged payload.
    $setupAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot '../MaxineSetup/Install-MPCVR-Maxine-RIFE.ps1'),[ref]$null,[ref]$null)
    foreach ($node in $setupAst.FindAll({param($item) $item -is [Management.Automation.Language.FunctionDefinitionAst] -and $item.Name -in @('New-RifeRuntimeBundle','Assert-RifeRuntimeDownloadContract')},$true)) {
        . ([scriptblock]::Create($node.Extent.Text))
    }
    Import-Module (Join-Path $installer 'RifeRuntimeManifest.psm1') -Force
    $payloadRoot=Join-Path $split 'payload'; New-Item -ItemType Directory $payloadRoot | Out-Null
    Get-ChildItem $nextBundle -Filter '*.zip' | Copy-Item -Destination $payloadRoot
    $rifeRuntimeChecksums=Join-Path $nextBundle 'SHA256SUMS.txt'
    $rifeRuntimeManifest=Join-Path $nextBundle 'runtime-manifest.json'
    $rifeRuntimeSource=Join-Path $payloadRoot 'unused-release.json'
    $coordinatorBundle=Join-Path $split 'coordinator-bundle'
    New-RifeRuntimeBundle -Destination $coordinatorBundle -ArchitectureKeys @('sm75')
    Check ((Get-Content (Join-Path $coordinatorBundle 'SHA256SUMS.txt')).Count -eq 3) 'Setup bundle does not list both shared packages and the GPU pack'
    Check ((Invoke-FixtureInstall $coordinatorBundle) -eq 0 -and [IO.File]::ReadAllText($installedDll) -eq 'build two') 'Setup bundle did not install'
    Remove-Item -LiteralPath (Join-Path $payloadRoot 'MPCVR-RIFE-Libraries.zip')
    $rejected=$false
    try { New-RifeRuntimeBundle -Destination (Join-Path $split 'incomplete-bundle') -ArchitectureKeys @('sm75') } catch { $rejected=$true }
    Check $rejected 'Setup built a bundle without its library package'
    'PASS: a new runtime downloads only its own package; split install, corruption, rollback, setup bundle and legacy migration.'
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
    if ($env:MPCVR_FIXTURE_KLITE_X64_ONLY -and $LiteralPath -like 'C:\Program Files (x86)\K-Lite Codec Pack\Filters\MPCVR*') { return $false }
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
    # K-Lite installed with only 64-bit components has no Filters\MPCVR.
    $env:MPCVR_FIXTURE_KLITE_X64_ONLY='1'
    try {
        & $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File $routingFixture -Root $fixture -Updater $updater
        $x64OnlyExit=$LASTEXITCODE
    } finally { Remove-Item Env:MPCVR_FIXTURE_KLITE_X64_ONLY -ErrorAction SilentlyContinue }
    Check ($x64OnlyExit -eq 0 -and (Test-Path -LiteralPath $marker)) "Updater refused a 64-bit-only K-Lite installation (host $hostExecutable, exit $x64OnlyExit)"
    Remove-Item -LiteralPath $marker -Force
    'PASS: updater delegates setup on a 64-bit-only K-Lite installation.'
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
