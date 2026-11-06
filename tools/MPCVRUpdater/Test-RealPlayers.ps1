#requires -Version 5.1

# Installs the real MPC-HC, MPC-BE and K-Lite Codec Pack, runs the updater on them with real
# registration, and plays a clip in each to see which renderer file the process actually loaded.
# It changes the machine it runs on, so it refuses to run anywhere but a disposable CI runner.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageArchive,
    [Parameter(Mandatory = $true)][string]$ChecksumFile,
    [Parameter(Mandatory = $true)][string]$WorkRoot
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 2.0

if ($env:GITHUB_ACTIONS -ne 'true') { throw 'This test installs players and changes filter registration. It only runs on CI runners.' }

Import-Module (Join-Path $PSScriptRoot 'MPCVRPlayerInstall.psm1') -Force
$updater = Join-Path $PSScriptRoot 'Update-MPCVR.ps1'
$hostExecutable = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$registration = New-MPCVRRegistrationProvider

function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-File([string]$Uri, [string]$Name) {
    $path = Join-Path $WorkRoot $Name
    if (-not (Test-Path -LiteralPath $path)) { Invoke-WebRequest -Uri $Uri -OutFile $path -UseBasicParsing -UserAgent 'Mozilla/5.0' }
    return $path
}
function Get-ReleaseAsset([string]$Repository, [string]$Pattern) {
    $headers = @{}
    if ($env:GH_TOKEN) { $headers.Authorization = "Bearer $env:GH_TOKEN" }
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/releases/latest" -Headers $headers
    $asset = @($release.assets | Where-Object { $_.name -like $Pattern })[0]
    "$Repository $($release.tag_name): $($asset.name)" | Write-Host
    return Get-File $asset.browser_download_url $asset.name
}
function Install-Silently([string]$Installer) {
    $process = Start-Process -FilePath $Installer -ArgumentList '/VERYSILENT', '/NORESTART', '/SUPPRESSMSGBOXES', '/SP-' -Wait -PassThru
    Check ($process.ExitCode -eq 0) "$Installer failed with exit code $($process.ExitCode)"
}
function Invoke-Updater {
    & $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File $updater -PackageArchive $PackageArchive -ChecksumFile $ChecksumFile -NoPause
    Check ($LASTEXITCODE -eq 0) "The updater failed with exit code $LASTEXITCODE"
}
function Stop-Players { Get-Process -Name (Get-MPCVRPlayerProcessName) -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }

function New-TestClip([string]$Path) {
    # Two seconds of uncompressed 64x64 RGB24 AVI; no decoder or download needed.
    $width = 64; $height = 64; $frames = 60; $rate = 30
    $frameSize = $width * $height * 3
    $stream = [IO.File]::Create($Path)
    $w = New-Object IO.BinaryWriter($stream)
    $ascii = { param($Text) $w.Write([Text.Encoding]::ASCII.GetBytes($Text)) }
    $moviSize = 4 + $frames * (8 + $frameSize)
    $hdrlSize = 4 + (8 + 56) + (8 + 4 + (8 + 56) + (8 + 40))
    $indexSize = 16 * $frames
    & $ascii 'RIFF'; $w.Write([int](4 + (8 + $hdrlSize) + (8 + $moviSize) + (8 + $indexSize))); & $ascii 'AVI '
    & $ascii 'LIST'; $w.Write([int]$hdrlSize); & $ascii 'hdrl'
    & $ascii 'avih'; $w.Write([int]56)
    foreach ($value in @([int](1000000 / $rate), ($frameSize * $rate), 0, 0x10, $frames, 0, 1, $frameSize, $width, $height, 0, 0, 0, 0)) { $w.Write([int]$value) }
    & $ascii 'LIST'; $w.Write([int](4 + (8 + 56) + (8 + 40))); & $ascii 'strl'
    & $ascii 'strh'; $w.Write([int]56); & $ascii 'vids'; $w.Write([int]0)
    foreach ($value in @(0, 0, 0, 1, $rate, 0, $frames, $frameSize, -1, 0)) { $w.Write([int]$value) }
    $w.Write([int16]0); $w.Write([int16]0); $w.Write([int16]$width); $w.Write([int16]$height)
    & $ascii 'strf'; $w.Write([int]40)
    foreach ($value in @(40, $width, $height)) { $w.Write([int]$value) }
    $w.Write([int16]1); $w.Write([int16]24)
    foreach ($value in @(0, $frameSize, 0, 0, 0, 0)) { $w.Write([int]$value) }
    & $ascii 'LIST'; $w.Write([int]$moviSize); & $ascii 'movi'
    $offsets = New-Object Collections.ArrayList
    $offset = 4
    for ($frame = 0; $frame -lt $frames; $frame++) {
        $pixels = New-Object byte[] $frameSize
        for ($index = 0; $index -lt $frameSize; $index++) { $pixels[$index] = [byte](($index + $frame * 7) % 256) }
        & $ascii '00db'; $w.Write([int]$frameSize); $w.Write($pixels)
        [void]$offsets.Add($offset)
        $offset += 8 + $frameSize
    }
    & $ascii 'idx1'; $w.Write([int]$indexSize)
    foreach ($item in $offsets) { & $ascii '00db'; $w.Write([int]0x10); $w.Write([int]$item); $w.Write([int]$frameSize) }
    $w.Dispose()
}

function Get-LoadedRenderer([string]$Executable, [string]$Clip, [int]$TimeoutSeconds = 45) {
    $process = Start-Process -FilePath $Executable -ArgumentList ('"{0}"' -f $Clip) -PassThru
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        while ([DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 500
            $process.Refresh()
            if ($process.HasExited) { throw "$Executable exited with code $($process.ExitCode) before the renderer loaded." }
            try {
                $module = @($process.Modules | Where-Object { $_.ModuleName -like 'MpcVideoRenderer*.ax' })
                if ($module.Count) { return $module[0].FileName }
            }
            catch { }
        }
        $process.Refresh()
        $modules = @($process.Modules | ForEach-Object { $_.ModuleName } | Where-Object { $_ -match '^(evr|quartz|d3d9|d3d11|mpc|LAV)' }) -join ', '
        $renderer = (Get-ItemProperty -Path 'HKCU:\Software\MPC-HC\MPC-HC\Settings' -Name 'DSVidRen' -ErrorAction SilentlyContinue).DSVidRen
        $ini = @(Get-ChildItem -LiteralPath ([IO.Path]::GetDirectoryName($Executable)) -Filter '*.ini' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) -join ', '
        throw "$Executable did not load MPC Video Renderer within $TimeoutSeconds seconds. Window: '$($process.MainWindowTitle)'. DSVidRen: $renderer. INI: '$ini'. Modules: $modules"
    }
    finally {
        Stop-Players
        Start-Sleep -Seconds 2
    }
}

New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null
$extract = Join-Path $WorkRoot 'package'
Expand-Archive -LiteralPath $PackageArchive -DestinationPath $extract -Force
$customX64 = Sha (Join-Path $extract 'MpcVideoRenderer64.ax')
$customX86 = Sha (Join-Path $extract 'MpcVideoRenderer.ax')
function Expected([string]$Architecture) { if ($Architecture -eq 'x64') { $customX64 } else { $customX86 } }

# --- Before any player exists the updater refuses, having changed nothing.
& $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File $updater -PackageArchive $PackageArchive -ChecksumFile $ChecksumFile -NoPause
Check ($LASTEXITCODE -ne 0) 'The updater succeeded with no player installed'
'PASS: no player is refused.'

# --- The real players, K-Lite first since it may hold back its MPC-HC when it finds another player.
# Its silent install selects no player by default and ignores /COMPONENTS; an INF names them.
$page = (Invoke-WebRequest -Uri 'https://www.codecguide.com/download_k-lite_codec_pack_mega.htm' -UseBasicParsing -UserAgent 'Mozilla/5.0').Content
$kliteUrl = [regex]::Match($page, 'https://[^"]*K-Lite_Codec_Pack_[^"]*_Mega\.exe').Value
Check ([bool]$kliteUrl) 'Could not find the K-Lite Mega download.'
Write-Host "K-Lite: $kliteUrl"
$kliteInf = Join-Path $WorkRoot 'klite.ini'
@(
    '[Setup]', 'Lang=en', "Dir=${env:ProgramFiles(x86)}\K-Lite Codec Pack", 'Group=K-Lite Codec Pack', 'NoIcons=0', 'SetupType=custom',
    'Components=player,player\mpchc,player\mpchc\x64,video,video\lav,audio,audio\lav,sourcefilter,sourcefilter\lav,other,other\mpcvr',
    'Tasks='
) | Set-Content -LiteralPath $kliteInf -Encoding ASCII
$kliteLog = Join-Path $WorkRoot 'klite-setup.log'
$process = Start-Process -FilePath (Get-File $kliteUrl 'K-Lite_Mega.exe') -ArgumentList '/VERYSILENT', '/NORESTART', '/SUPPRESSMSGBOXES', '/SP-', "/LOADINF=`"$kliteInf`"", "/LOG=`"$kliteLog`"" -Wait -PassThru
Check ($process.ExitCode -eq 0) "K-Lite setup failed with exit code $($process.ExitCode)"
Get-Content -LiteralPath $kliteLog | Select-String -Pattern 'Selected components' | ForEach-Object { Write-Host $_.Line }

$hc = Get-ReleaseAsset 'clsid2/mpc-hc' 'MPC-HC.*.x64.exe'
Install-Silently $hc
foreach ($pattern in @('MPC-BE.*.x64-installer.zip', 'MPC-BE.*.x86-installer.zip')) {
    $zip = Get-ReleaseAsset 'Aleksoid1978/MPC-BE' $pattern
    $folder = Join-Path $WorkRoot ([IO.Path]::GetFileNameWithoutExtension($zip))
    Expand-Archive -LiteralPath $zip -DestinationPath $folder -Force
    Install-Silently (@(Get-ChildItem -LiteralPath $folder -Recurse -Filter '*.exe')[0].FullName)
}

$targets = @(Get-MPCVRPlayerTarget)
$targets | Format-Table Player, Architecture, LoadMode, Destination -AutoSize | Out-String -Width 220 | Write-Host
# K-Lite decides its own selection, so if it still left MPC-HC out, say so and test what it did install.
if (-not @($targets | Where-Object { $_.Player -eq 'K-Lite Codec Pack (MPC-HC)' }).Count) {
    Write-Host '::warning::K-Lite silently left out its MPC-HC; its shared filters are tested instead of its player.'
    Check (@($targets | Where-Object { $_.Player -eq 'K-Lite Codec Pack (filter)' }).Count -gt 0) 'K-Lite installed neither MPC-HC nor a renderer'
}
foreach ($player in @('MPC-HC', 'MPC-BE')) {
    Check (@($targets | Where-Object { $_.Player -eq $player -and $_.Architecture -eq 'x64' }).Count -eq 1) "Real $player x64 was not detected exactly once"
}
Check (@($targets | Where-Object { $_.Player -eq 'MPC-BE' -and $_.Architecture -eq 'x86' }).Count -eq 1) 'Real MPC-BE x86 was not detected'
foreach ($target in @($targets | Where-Object { $_.Player -eq 'MPC-BE' })) {
    Check (Test-Path -LiteralPath $target.Destination -PathType Leaf) "MPC-BE's bundled renderer is not where this expects it: $($target.Destination)"
}
$beX64 = @($targets | Where-Object { $_.Player -eq 'MPC-BE' -and $_.Architecture -eq 'x64' })[0]

# Keep a stock renderer, to make it the registered one below.
$stock = Join-Path $WorkRoot 'stock\MpcVideoRenderer64.ax'
New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($stock)) -Force | Out-Null
Copy-Item -LiteralPath $beX64.Destination -Destination $stock
Check ((Sha $stock) -ne $customX64) 'The stock MPC-BE renderer already matches this build'

# --- The updater, with real registration.
$before = @{ x64 = (& $registration.Read 'x64'); x86 = (& $registration.Read 'x86') }
Write-Host "Registered before: x64 '$($before.x64)', x86 '$($before.x86)'"
Invoke-Updater
foreach ($target in $targets) {
    Check ((Sha $target.Destination) -eq (Expected $target.Architecture)) "$($target.Destination) does not hold the new build"
}
foreach ($architecture in @('x64', 'x86')) {
    $registered = & $registration.Read $architecture
    Check ([bool]$registered) "Nothing is registered for $architecture"
    Check ((Sha $registered) -eq (Expected $architecture)) "$architecture registration points at $registered, which is not this build"
}
Check (@(Get-ChildItem -LiteralPath $beX64.Destination -ErrorAction Stop).Count -eq 1) 'renderer vanished'
'PASS: all real players updated, registration on this build.'

# A registration on a stock file elsewhere: real regsvr32 hands it to MPC-BE, and verifies it.
& $registration.Register 'x64' $stock
Check ((& $registration.Read 'x64') -eq $stock) 'Could not prepare a stock registration'
Invoke-Updater
Check ((& $registration.Read 'x64') -ieq $beX64.Destination) 'The updater left MPC-BE on a stock registration'
'PASS: a stock registration elsewhere is replaced with real regsvr32.'

# --- Which file does each real player load? MPC-HC is given a stock registration on purpose, to
# show it loads its own folder's copy regardless. K-Lite's MPC-HC shares the setting.
$clip = Join-Path $WorkRoot 'clip.avi'
New-TestClip $clip
$settings = 'HKCU:\Software\MPC-HC\MPC-HC\Settings'
New-Item -Path $settings -Force | Out-Null
New-ItemProperty -Path $settings -Name 'DSVidRen' -PropertyType DWord -Value 14 -Force | Out-Null

$beLoaded = Get-LoadedRenderer $beX64.Executable $clip
Write-Host "MPC-BE loaded: $beLoaded"
Check ($beLoaded -ieq $beX64.Destination) "MPC-BE loaded $beLoaded"
Check ((Sha $beLoaded) -eq $customX64) 'MPC-BE loaded a renderer that is not this build'

& $registration.Register 'x64' $stock
foreach ($target in @($targets | Where-Object { $_.Architecture -eq 'x64' -and $_.LoadMode -eq 'PlayerFolder' })) {
    # A player that was killed may have rewritten or skipped settings, so set the renderer each time.
    New-ItemProperty -Path $settings -Name 'DSVidRen' -PropertyType DWord -Value 14 -Force | Out-Null
    $loaded = Get-LoadedRenderer $target.Executable $clip
    Write-Host "$($target.Player) loaded: $loaded"
    Check ($loaded -ieq $target.Destination) "$($target.Player) loaded $loaded instead of $($target.Destination)"
    Check ((Sha $loaded) -eq $customX64) "$($target.Player) loaded a renderer that is not this build"
}
Invoke-Updater
'PASS: each real player loaded this build, from the expected file.'

# --- The shortcut, a shortcut's old script name, and the manual installer.
& $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Install-MPCVRUpdater.ps1') -NoPause
Check ($LASTEXITCODE -eq 0) 'The shortcut installer failed'
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Restore MPC-VR Maxine + RIFE.lnk'
$shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath)
$installed = Join-Path $env:LOCALAPPDATA 'MPCVR Custom Updater'
Check ($shortcut.Arguments -like "*$installed\Update-MPCVR.ps1*") "Shortcut runs: $($shortcut.Arguments)"
& $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $installed 'Update-KLiteMPCVR.ps1') -PackageArchive $PackageArchive -ChecksumFile $ChecksumFile -NoPause
Check ($LASTEXITCODE -eq 0) 'The old updater name did not work from the installed folder'

Remove-Item -LiteralPath $beX64.Destination -Force
& cmd.exe /d /c "`"$extract\Install_MPCVR_64.cmd`" < NUL"
Check ((Test-Path -LiteralPath $beX64.Destination -PathType Leaf) -and (Sha $beX64.Destination) -eq $customX64) 'The manual installer did not restore MPC-BE'
Check ((Sha (& $registration.Read 'x64')) -eq $customX64) 'The manual installer left another renderer registered'
'PASS: shortcut, old updater name and manual installer.'
'Real player validation passed.'
