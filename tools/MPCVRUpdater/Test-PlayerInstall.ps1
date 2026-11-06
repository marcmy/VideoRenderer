#requires -Version 5.1

# Player detection and the install transaction on fixture folders, with registration replaced by a
# fake. Changes nothing outside a temporary folder. tools/MPCVRUpdater/Test-RealPlayers.ps1 covers
# real players and real registration, on disposable CI machines only.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Import-Module (Join-Path $PSScriptRoot 'MPCVRPlayerInstall.psm1') -Force

$root = Join-Path ([IO.Path]::GetTempPath()) ("mpcvr-player-test-" + [guid]::NewGuid().ToString('N'))

function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Touch([string]$Path, [string]$Value = 'stock') {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($Path)) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Value)
}
function Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function New-Provider([hashtable]$State, [string]$FailRegister = '') {
    # Closures over $State and $FailRegister, as the real one reads what regsvr32 wrote.
    return @{
        Read = { param($Architecture) $State[$Architecture] }.GetNewClosure()
        Register = {
            param($Architecture, $Path)
            if ($FailRegister -and $Path -like $FailRegister) { throw 'fixture regsvr32 failure' }
            $State[$Architecture] = $Path
            $State.Calls = @($State.Calls) + "register $Architecture $Path"
        }.GetNewClosure()
        Unregister = {
            param($Architecture, $Path)
            $State.Remove($Architecture)
            $State.Calls = @($State.Calls) + "unregister $Architecture $Path"
        }.GetNewClosure()
    }
}

try {
    $sources = @{
        'MpcVideoRenderer.ax' = (Join-Path $root 'package\MpcVideoRenderer.ax')
        'MpcVideoRenderer64.ax' = (Join-Path $root 'package\MpcVideoRenderer64.ax')
    }
    Touch $sources['MpcVideoRenderer.ax'] 'custom x86'
    Touch $sources['MpcVideoRenderer64.ax'] 'custom x64'
    $customX64 = Sha $sources['MpcVideoRenderer64.ax']
    $customX86 = Sha $sources['MpcVideoRenderer.ax']

    # --- Detection ---
    $programs = Join-Path $root 'ProgramFiles'
    $programsX86 = Join-Path $root 'ProgramFiles (x86)'
    $roots = @($programs, $programsX86)
    Check (@(Get-MPCVRPlayerTarget -ProgramRoot $roots -Hint @()).Count -eq 0) 'Empty folders detected a player'
    $rejected = $false
    try { [void](Get-MPCVRRequiredPlayerTarget -ProgramRoot $roots -Hint @()) } catch { $rejected = $_.Exception.Message -like '*No supported player*' }
    Check $rejected 'No player found did not explain itself'

    # K-Lite, in the layouts it really has: 64-bit MPC-HC and the 32-bit shared filter.
    $klite = Join-Path $programsX86 'K-Lite Codec Pack'
    Touch (Join-Path $klite 'MPC-HC64\mpc-hc64.exe')
    Touch (Join-Path $klite 'MPC-HC64\MPCVR\MpcVideoRenderer64.ax')
    Touch (Join-Path $klite 'Filters\MPCVR\MpcVideoRenderer.ax')
    $kliteTargets = @(Get-MPCVRPlayerTarget -ProgramRoot $roots -Hint @())
    Check ($kliteTargets.Count -eq 2) "Default K-Lite should give two targets, got $($kliteTargets.Count)"
    Check (@($kliteTargets | Where-Object { $_.LoadMode -eq 'PlayerFolder' -and $_.Architecture -eq 'x64' -and $_.Player -like 'K-Lite*' -and $_.Destination -like '*\MPC-HC64\MPCVR\MpcVideoRenderer64.ax' }).Count -eq 1) 'K-Lite x64 target'
    Check (@($kliteTargets | Where-Object { $_.LoadMode -eq 'SharedFilter' -and $_.Architecture -eq 'x86' -and $_.Destination -like '*\Filters\MPCVR\MpcVideoRenderer.ax' }).Count -eq 1) 'K-Lite x86 filter target'
    $x64Only = Join-Path $root 'x64-only'
    Touch (Join-Path $x64Only 'K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax')
    Check (@(Get-MPCVRPlayerTarget -ProgramRoot @($x64Only) -Hint @()).Count -eq 1) 'A 64-bit-only K-Lite installation should give only its x64 renderer'

    # K-Lite in a custom folder, from its uninstall entry, and its MPC-HC's HKCU ExePath: not standalone.
    $customKLite = Join-Path $root 'Codecs\KLite'
    Touch (Join-Path $customKLite 'MPC-HC64\mpc-hc64.exe')
    $hints = @(
        [pscustomobject]@{ Kind = 'K-Lite'; Path = "$customKLite\" },
        [pscustomobject]@{ Kind = 'MPC-HC'; Path = (Join-Path $customKLite 'MPC-HC64\mpc-hc64.exe') }
    )
    $custom = @(Get-MPCVRPlayerTarget -ProgramRoot @() -Hint $hints)
    Check ($custom.Count -eq 1 -and $custom[0].Player -like 'K-Lite*') 'K-Lite in a custom folder was misclassified'
    Check ($custom[0].Destination -eq (Join-Path $customKLite 'MPC-HC64\MPCVR\MpcVideoRenderer64.ax')) 'K-Lite in a custom folder has the wrong target'

    # Standalone MPC-HC and MPC-BE, both architectures, default and recorded folders.
    Touch (Join-Path $programs 'MPC-HC\mpc-hc64.exe')
    Touch (Join-Path $programsX86 'MPC-HC\mpc-hc.exe')
    Touch (Join-Path $programs 'MPC-BE\mpc-be64.exe')
    Touch (Join-Path $programs 'MPC-BE\Filters\MpcVideoRenderer64.ax')
    Touch (Join-Path $programsX86 'MPC-BE\mpc-be.exe')
    Touch (Join-Path $programs 'MPC-BE x64\mpc-be64.exe')
    $beElsewhere = Join-Path $root 'Apps\BE'
    Touch (Join-Path $beElsewhere 'mpc-be64.exe')
    $all = @(Get-MPCVRPlayerTarget -ProgramRoot $roots -Hint @(
        [pscustomobject]@{ Kind = 'MPC-BE'; Path = (Join-Path $beElsewhere 'mpc-be64.exe') },
        [pscustomobject]@{ Kind = 'MPC-BE'; Path = (Join-Path $programs 'MPC-BE\mpc-be64.exe') }
    ))
    $find = { param($Player, $Destination) @($all | Where-Object { $_.Player -eq $Player -and $_.Destination -eq $Destination }) }
    Check (@(& $find 'MPC-HC' (Join-Path $programs 'MPC-HC\MPCVR\MpcVideoRenderer64.ax')).Count -eq 1) 'Standalone MPC-HC x64 target'
    Check (@(& $find 'MPC-HC' (Join-Path $programsX86 'MPC-HC\MPCVR\MpcVideoRenderer.ax')).Count -eq 1) 'Standalone MPC-HC x86 target'
    Check (@(& $find 'MPC-BE' (Join-Path $programs 'MPC-BE\Filters\MpcVideoRenderer64.ax')).Count -eq 1) 'MPC-BE x64 target, found twice or not at all'
    Check (@(& $find 'MPC-BE' (Join-Path $programsX86 'MPC-BE\Filters\MpcVideoRenderer.ax')).Count -eq 1) 'MPC-BE x86 target'
    Check (@(& $find 'MPC-BE' (Join-Path $programs 'MPC-BE x64\Filters\MpcVideoRenderer64.ax')).Count -eq 1) 'Older MPC-BE x64 folder'
    Check (@(& $find 'MPC-BE' (Join-Path $beElsewhere 'Filters\MpcVideoRenderer64.ax')).Count -eq 1) 'MPC-BE from its recorded path'
    Check (@($all | Where-Object { $_.Player -eq 'MPC-BE' -and $_.LoadMode -ne 'Registration' }).Count -eq 0) 'MPC-BE must depend on registration'
    Check (@($all | Where-Object { $_.Player -eq 'MPC-HC' -and $_.LoadMode -ne 'PlayerFolder' }).Count -eq 0) 'MPC-HC must load from its folder'
    'PASS: detection of K-Lite, standalone MPC-HC and MPC-BE, in both architectures and custom folders.'

    # --- All three together, the K-Lite registration already being replaced by this build ---
    $mixed = @($all | Where-Object { $_.Destination -notlike "*\MPC-BE x64\*" -and $_.Destination -notlike "$beElsewhere*" -and $_.Destination -notlike "$programsX86\MPC-BE*" })
    $kliteX64 = Join-Path $klite 'MPC-HC64\MPCVR\MpcVideoRenderer64.ax'
    $state = @{ Calls = @(); x64 = $kliteX64 }
    $installed = @(Install-MPCVRPlayerRenderer -Sources $sources -Targets $mixed -Registration (New-Provider $state))
    Check ($installed.Count -eq $mixed.Count) 'Not every target was reported'
    foreach ($target in $mixed) {
        $want = if ($target.Architecture -eq 'x64') { $customX64 } else { $customX86 }
        Check ((Sha $target.Destination) -eq $want) "$($target.Destination) does not hold the new build"
    }
    Check (Test-Path -LiteralPath (Join-Path $programs 'MPC-HC\MPCVR\MpcVideoRenderer64.ax') -PathType Leaf) 'Standalone MPC-HC without an MPCVR folder must get one'
    Check ($state.x64 -eq $kliteX64 -and @($state.Calls).Count -eq 0) 'A registration already on this build must not be moved'
    $beRow = @($installed | Where-Object { $_.Player -eq 'MPC-BE' })[0]
    Check ($beRow.LoadsFrom -eq $kliteX64) 'MPC-BE should be reported as loading the registered file'
    Check (@(Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.mpcvr-backup-*').Count -eq 0) 'Backups left behind'
    'PASS: all three installed in one transaction, keeping a registration already on this build.'

    # --- MPC-BE and a registration on a stock file elsewhere ---
    $alone = Join-Path $root 'alone'
    $stock = Join-Path $root 'SomeTool\MpcVideoRenderer64.ax'
    Touch $stock 'stock x64'
    Touch (Join-Path $alone 'MPC-BE\mpc-be64.exe')
    Touch (Join-Path $alone 'MPC-BE\Filters\MpcVideoRenderer64.ax')
    $be = @(Get-MPCVRPlayerTarget -ProgramRoot @($alone) -Hint @())
    $beFile = $be[0].Destination
    $state = @{ Calls = @(); x64 = $stock }
    [void](Install-MPCVRPlayerRenderer -Sources $sources -Targets $be -Registration (New-Provider $state))
    Check ($state.x64 -eq $beFile) 'MPC-BE would load the stock renderer registered elsewhere'
    Check ((Sha $stock) -ne $customX64) 'A file outside the targets was changed'

    # Nothing registered: a portable MPC-BE, no Filters folder.
    $portable = Join-Path $root 'portable'
    Touch (Join-Path $portable 'MPC-BE\mpc-be64.exe')
    $be = @(Get-MPCVRPlayerTarget -ProgramRoot @($portable) -Hint @())
    $state = @{ Calls = @() }
    [void](Install-MPCVRPlayerRenderer -Sources $sources -Targets $be -Registration (New-Provider $state))
    Check ($state.x64 -eq $be[0].Destination) 'MPC-BE with nothing registered must be registered'
    'PASS: MPC-BE takes over a stale or missing registration.'

    # Registering a file that doesn't take effect must not pass.
    $state = @{ Calls = @() }
    $silent = @{ Read = { param($Architecture) $null }; Register = { param($Architecture, $Path) }; Unregister = { param($Architecture, $Path) } }
    $failed = $false
    try { [void](Install-MPCVRPlayerRenderer -Sources $sources -Targets $be -Registration $silent) } catch { $failed = $_.Exception.Message -like '*registered as*' }
    Check $failed 'A registration that did not take effect was accepted'

    # --- Rollback ---
    # An existing renderer locked by another process, after others already changed.
    $tx = Join-Path $root 'tx'
    Touch (Join-Path $tx 'K-Lite Codec Pack\MPC-HC64\mpc-hc64.exe')
    $txKLite = Join-Path $tx 'K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax'
    Touch $txKLite 'stock klite'
    Touch (Join-Path $tx 'MPC-HC\mpc-hc64.exe')
    Touch (Join-Path $tx 'MPC-BE\mpc-be64.exe')
    $txBe = Join-Path $tx 'MPC-BE\Filters\MpcVideoRenderer64.ax'
    Touch $txBe 'stock be'
    $txTargets = @(Get-MPCVRPlayerTarget -ProgramRoot @($tx) -Hint @())
    Check ($txTargets.Count -eq 3) "Rollback fixture needs 3 targets, got $($txTargets.Count)"
    $oldKLite = Sha $txKLite
    $oldBe = Sha $txBe
    $newFolder = Join-Path $tx 'MPC-HC\MPCVR'
    $lock = [IO.File]::Open($txBe, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
    try {
        $state = @{ Calls = @(); x64 = $txBe }
        $failed = $false
        try { [void](Install-MPCVRPlayerRenderer -Sources $sources -Targets $txTargets -Registration (New-Provider $state)) } catch { $failed = $true }
    }
    finally { $lock.Dispose() }
    Check $failed 'A locked renderer unexpectedly succeeded'
    Check ((Sha $txKLite) -eq $oldKLite) 'Rollback did not restore K-Lite'
    Check ((Sha $txBe) -eq $oldBe) 'Rollback changed the locked MPC-BE renderer'
    Check (-not (Test-Path -LiteralPath $newFolder)) 'Rollback left a new MPCVR folder behind'

    # A failed registration undoes the files and restores the previous registration.
    $state = @{ Calls = @(); x64 = $stock }
    $failed = $false
    try { [void](Install-MPCVRPlayerRenderer -Sources $sources -Targets $txTargets -Registration (New-Provider $state "*\MPC-BE\*")) } catch { $failed = $true }
    Check $failed 'A failed registration unexpectedly succeeded'
    Check ($state.x64 -eq $stock) 'Rollback lost the previous registration'
    Check ((Sha $txKLite) -eq $oldKLite -and (Sha $txBe) -eq $oldBe) 'A failed registration kept new files'
    Check (-not (Test-Path -LiteralPath $newFolder)) 'A failed registration kept a new folder'

    # After a later failure, with nothing registered before: unregistered again.
    $withBroken = @($txTargets) + [pscustomobject]@{ Player = 'Fixture'; Architecture = 'x64'; Executable = $null; FileName = 'Missing.ax'; Destination = (Join-Path $tx 'x\Missing.ax'); LoadMode = 'PlayerFolder' }
    $failed = $false
    try { [void](Install-MPCVRPlayerRenderer -Sources $sources -Targets $withBroken -Registration (New-Provider @{ Calls = @() })) } catch { $failed = $_.Exception.Message -like '*does not contain Missing.ax*' }
    Check $failed 'A package missing a needed file was accepted'
    Check ((Sha $txKLite) -eq $oldKLite) 'A package missing a file changed something'

    $state = @{ Calls = @() }
    $verifyFails = @{
        Read = { param($Architecture) if ($state.ContainsKey($Architecture)) { if ($state.Reads++ -ge 1) { Join-Path $tx 'gone.ax' } else { $state[$Architecture] } } }.GetNewClosure()
        Register = { param($Architecture, $Path) $state[$Architecture] = $Path; $state.Reads = 0 }.GetNewClosure()
        Unregister = { param($Architecture, $Path) $state.Remove($Architecture); $state.Unregistered = $Path }.GetNewClosure()
    }
    $failed = $false
    try { [void](Install-MPCVRPlayerRenderer -Sources $sources -Targets $txTargets -Registration $verifyFails) } catch { $failed = $_.Exception.Message -like '*would not load this build*' }
    Check $failed 'A player left loading another file was accepted'
    Check ($state.Unregistered -eq $txBe -and -not $state.ContainsKey('x64')) 'Rollback did not unregister what it registered'
    Check (@(Get-ChildItem -LiteralPath $tx -Recurse -File -Filter '*.mpcvr-backup-*').Count -eq 0) 'Rollback left backups behind'
    'PASS: rollback restores files, new folders and the previous registration.'

    # Repeating an install changes nothing.
    $state = @{ Calls = @(); x64 = $stock }
    [void](Install-MPCVRPlayerRenderer -Sources $sources -Targets $txTargets -Registration (New-Provider $state))
    $before = @($txTargets | ForEach-Object { Sha $_.Destination })
    $calls = @($state.Calls).Count
    [void](Install-MPCVRPlayerRenderer -Sources $sources -Targets $txTargets -Registration (New-Provider $state))
    Check (@($txTargets | ForEach-Object { Sha $_.Destination }) -join ',' -eq ($before -join ',')) 'A repeated install changed hashes'
    Check (@($state.Calls).Count -eq $calls) 'A repeated install registered again'

    # The installed updater folder has what the shortcut and old pinned shortcuts run.
    foreach ($name in @('Update-MPCVR.ps1', 'Update-KLiteMPCVR.ps1', 'MPCVRPlayerInstall.psm1', 'Install-MPCVRUpdater.ps1')) {
        Check (Test-Path -LiteralPath (Join-Path $PSScriptRoot $name) -PathType Leaf) "$name is missing"
    }
    $forwarder = Join-Path $root 'forward'
    New-Item -ItemType Directory -Path $forwarder | Out-Null
    Copy-Item (Join-Path $PSScriptRoot 'Update-KLiteMPCVR.ps1') $forwarder
    Set-Content -LiteralPath (Join-Path $forwarder 'Update-MPCVR.ps1') -Value 'param([switch]$NoPause,[switch]$ValidateOnly) if ($NoPause -and $ValidateOnly) { exit 7 } else { exit 1 }'
    $hostExecutable = if ($PSVersionTable.PSEdition -eq 'Core') { (Get-Command pwsh.exe).Source } else { Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe' }
    & $hostExecutable -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $forwarder 'Update-KLiteMPCVR.ps1') -NoPause -ValidateOnly
    Check ($LASTEXITCODE -eq 7) "Old updater name did not forward (exit $LASTEXITCODE)"
    'PASS: repeated installs are stable and the old updater name forwards.'
    "Player install tests passed under PowerShell $($PSVersionTable.PSVersion)."
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
