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
        schemaVersion=2;runtimeAbi=2;tensorRtVersion='11.3.0';tensorRtMajorMinor='11.3';cudaVersion='13.4';cudaRuntimeVersion='13.4'
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
