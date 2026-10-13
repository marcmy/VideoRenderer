#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Position=0)][ValidateSet('status','sync','build','test','bundle','publish','verify','dependencies')][string]$Command='status',
    [ValidateSet('x64','x86','both')][string]$Architecture='x64',
    [ValidateSet('auto','release','local','test')][string]$Flavor='local',
    [string]$Output,
    [string]$Component,
    [switch]$Offline
)
$ErrorActionPreference='Stop'
$repoRoot=$PSScriptRoot
Set-Location -LiteralPath $repoRoot
function Run-Git([string[]]$Arguments) {
    & git -C $repoRoot @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git failed: $Arguments" }
}
function Run-Python([string[]]$Arguments) {
    $launcher=Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) { & $launcher.Source -3 @Arguments } else { & python @Arguments }
    if ($LASTEXITCODE -ne 0) { throw "Python command failed: $Arguments" }
}
function Fetch-State {
    foreach ($remote in @('origin','upstream')) {
        $expected=if ($remote -eq 'origin') {'marcmy/VideoRenderer'} else {'Aleksoid1978/VideoRenderer'}
        $url=(& git remote get-url $remote).Trim()
        if ($LASTEXITCODE -ne 0 -or $url -notmatch ('^(https://github\.com/|git@github\.com:)'+[regex]::Escape($expected)+'(\.git)?/?$')) { throw "Unexpected $remote repository: $url" }
    }
    Run-Git @('fetch','origin')
    Run-Git @('fetch','upstream','--no-tags','master')
}
function Prepare-Tools {
    $directory=Join-Path $repoRoot '_tools\nasm'
    $executable=Join-Path $directory 'nasm-3.01\nasm.exe'
    if (-not (Test-Path -LiteralPath $executable)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        $archive=Join-Path $directory 'nasm-3.01-win64.zip'
        if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest 'https://www.nasm.us/pub/nasm/releasebuilds/3.01/win64/nasm-3.01-win64.zip' -OutFile $archive -UseBasicParsing }
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine 'e0ba5157007abc7b1a65118a96657a961ddf55f7e3f632ee035366dfce039ca4') { throw 'NASM checksum mismatch' }
        Expand-Archive -LiteralPath $archive -DestinationPath $directory -Force
    }
    $env:PATH=(Split-Path $executable)+';'+$env:PATH
}
function Build-Renderer {
    Prepare-Tools
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs=(& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (-not $vs) { throw 'Visual Studio C++ tools were not found.' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    Set-Location -LiteralPath $repoRoot
    & (Join-Path $repoRoot 'tools\Dev\Version.ps1') -Flavor $Flavor -Generate | Out-Null
    $architectures=if ($Architecture -eq 'both') {@('x86','x64')} else {@($Architecture)}
    foreach ($arch in $architectures) {
        $platform=$arch
        & msbuild MpcVideoRenderer.sln /nologo /m /nr:false /p:Configuration=Release "/p:Platform=$platform" /v:minimal
        if ($LASTEXITCODE -ne 0) { throw "$arch renderer build failed." }
    }
    $metadata=Get-Content '_bin\source-version.json' -Raw | ConvertFrom-Json
    foreach ($arch in $architectures) {
        $name=if ($arch -eq 'x64') {'MpcVideoRenderer64.ax'} else {'MpcVideoRenderer.ax'}
        $path=Join-Path $repoRoot "_bin\Filter_$arch\$name"
        $version=[Diagnostics.FileVersionInfo]::GetVersionInfo($path)
        if ($version.FileVersion -ne $metadata.version -or $version.ProductVersion -ne $metadata.productVersion) { throw "Compiled version disagrees with source metadata: $path" }
        if (-not $version.Comments.Contains($metadata.commit) -or -not $version.Comments.Contains($metadata.upstreamCommit)) { throw "Compiled source provenance is missing: $path" }
        Write-Host "$name : $($version.ProductVersion)"
    }
}
function Test-Renderer {
    Prepare-Tools
    Run-Python @('tools/Dev/test_release.py')
    foreach ($hostName in @('powershell.exe','pwsh.exe')) {
        & $hostName -NoLogo -NoProfile -ExecutionPolicy Bypass -File tools/Dev/Test-Version.ps1
        if ($LASTEXITCODE -ne 0) { throw "$hostName version tests failed." }
        & $hostName -NoLogo -NoProfile -ExecutionPolicy Bypass -File tools/Dev/Test-Dependencies.ps1
        if ($LASTEXITCODE -ne 0) { throw "$hostName dependency tests failed." }
    }
    $pythonTests=@('rife-receive-pacing-test/test_receive_pacing.py','rife-lifecycle-test/test_rife_lifecycle.py','rife-spatial-contract-test/test_rife_vsr_routing.py','rife-presentation-preparation-test/test_preparation_contract.py','rife-spatial-contract-test/test_rife_spatial_contract.py')
    foreach ($path in $pythonTests) { Run-Python @("tools/$path") }
    $nativeTests=@('rife-scheduler-test/build-test.cmd','rife-runtime-abi-test/build-test.cmd','rife-cache-test/build-test.cmd','rife-image-scene-test/build-test.cmd','rife-nvof-input-test/build-test.cmd','rife-spatial-policy-test/build-test.cmd','rife-svp-scene-test/build-test.cmd','rife-precision-test/build-warp.cmd')
    foreach ($path in $nativeTests) {
        & cmd.exe /d /c "tools\$($path.Replace('/','\'))"
        if ($LASTEXITCODE -ne 0) { throw "Regression test failed: $path" }
    }
    if (Test-Path '_bin\Filter_x64\MpcVideoRenderer64.ax') {
        & cmd.exe /d /c tools\rife-working-resolution-test\build-test.cmd
        if ($LASTEXITCODE -ne 0) { throw 'Compiled dialog test failed.' }
    }
    Run-Python @('tools/Dev/dependencies.py','check','--lock','dependencies.lock.json')
}
$previousFlavor=$env:MPCVR_BUILD_FLAVOR
try {
    $env:MPCVR_BUILD_FLAVOR=$Flavor
    switch ($Command) {
        'status' {
            if (-not $Offline) { Fetch-State }
            Run-Git @('status','--short','--branch')
            Run-Git @('rev-list','--left-right','--count','HEAD...origin/master')
            Run-Git @('rev-list','--count','HEAD..upstream/master')
            & tools/Dev/Version.ps1 -Flavor $Flavor
        }
        'sync' {
            Fetch-State
            if ((& git status --porcelain).Count) { throw 'Preserve and commit intentional work before integrating; use a temporary integration worktree for a dirty checkout.' }
            if ((& git branch --show-current).Trim() -ne 'master') { throw 'Routine sync runs from canonical master; review feature-branch divergence separately.' }
            Run-Git @('merge','--ff-only','origin/master')
            $pending=[int](& git rev-list --count HEAD..upstream/master)
            if ($pending) {
                Run-Git @('merge','--no-ff','--no-commit','upstream/master')
                & tools/Dev/Version.ps1 -RecordUpstream upstream/master
                Run-Git @('add','upstream.version.json')
                Run-Git @('commit','-m','Merge upstream VideoRenderer and record its source revision')
                Write-Host 'Upstream integrated. Run build -Architecture both, test, then publish. Conflicts remain reviewable; no automatic resolver rewrites source.'
            }
        }
        'build' { Build-Renderer }
        'test' { Test-Renderer }
        'bundle' {
            Build-Renderer
            $metadata=Get-Content '_bin/source-version.json' -Raw | ConvertFrom-Json
            if (-not $Output) { $Output=Join-Path $repoRoot ('_artifacts/'+$metadata.sourceId+'/'+$Architecture) }
            Run-Python @('tools/Dev/release.py','package','--version','_bin/source-version.json','--architecture',$Architecture,'--output',$Output)
        }
        'publish' {
            Fetch-State
            if ((& git status --porcelain).Count -or (& git branch --show-current).Trim() -ne 'master') { throw 'Publish requires committed canonical master; finish reviewing your changes first.' }
            $env:MPCVR_BUILD_FLAVOR='release'; $Flavor='release'; $Architecture='both'
            Build-Renderer; Test-Renderer
            $alreadyPushed=(& git rev-parse HEAD).Trim() -eq (& git rev-parse origin/master).Trim()
            Run-Git @('push','origin','HEAD:master')
            if ($alreadyPushed) {
                & gh workflow run main.yml --repo marcmy/VideoRenderer --ref master
                if ($LASTEXITCODE -ne 0) { throw 'Source is already pushed, but release retry dispatch failed.' }
            }
            Write-Host 'Source pushed normally. main.yml publishes the verified source revision automatically.'
        }
        'verify' { Run-Python @('tools/Dev/release.py','verify-published') }
        'dependencies' {
            if ($Component -notin @('rife-runtime','rife-models','maxine')) { throw 'Specify -Component rife-runtime, rife-models, or maxine.' }
            if ((& git status --porcelain).Count) { throw 'Dependency publishing requires a committed source revision.' }
            $sha=(& git rev-parse HEAD).Trim()
            & gh api "repos/marcmy/VideoRenderer/commits/$sha" --silent
            if ($LASTEXITCODE -ne 0) { throw 'Push this committed source to a review branch before dispatching its dependency build.' }
            & gh workflow run publish.yml --repo marcmy/VideoRenderer-Dependencies -f "component=$Component" -f "source_ref=$sha"
            if ($LASTEXITCODE -ne 0) { throw 'Dependency workflow dispatch failed.' }
            Write-Host 'Dependency publishing dispatched separately. After validation, refresh the lock with dependencies.py plan and lock, review and publish the source update.'
        }
    }
} finally { $env:MPCVR_BUILD_FLAVOR=$previousFlavor }
