Set-StrictMode -Version 2.0

# How each player finds the renderer, from their source:
# - MPC-HC, standalone or from K-Lite, first loads <exe folder>\MPCVR\<renderer> by path, whatever is
#   registered, and uses the registered filter only if that file is missing.
# - MPC-BE only ever creates the registered CLSID. Its installer puts the renderer in <exe folder>\Filters
#   and registers it, but only one file per architecture can be registered on a machine.
$script:RendererClsid = '{71F080AA-8661-4093-B15E-4F6903E77D0A}'
$script:FilterCategory = '{083863F1-70DE-11D0-BD40-00A0C911CE86}'
$script:RendererFiles = @{ x64 = 'MpcVideoRenderer64.ax'; x86 = 'MpcVideoRenderer.ax' }
$script:PlayerExecutables = @{
    'MPC-HC' = @(@{ Exe = 'mpc-hc64.exe'; Architecture = 'x64' }, @{ Exe = 'mpc-hc.exe'; Architecture = 'x86' })
    'MPC-BE' = @(@{ Exe = 'mpc-be64.exe'; Architecture = 'x64' }, @{ Exe = 'mpc-be.exe'; Architecture = 'x86' })
}

function Get-MPCVRPlayerProcessName {
    return @('mpc-hc', 'mpc-hc64', 'mpc-be', 'mpc-be64')
}

function Read-RegistryValue {
    param(
        [Microsoft.Win32.RegistryHive]$Hive,
        [Microsoft.Win32.RegistryView]$View,
        [string]$SubKey,
        [string]$Name
    )

    $base = $null
    $key = $null
    try {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($Hive, $View)
        $key = $base.OpenSubKey($SubKey)
        if ($null -eq $key) { return $null }
        $value = $key.GetValue($Name)
        if ($null -eq $value) { return $null }
        return ([string]$value).Trim().Trim('"')
    }
    catch {
        return $null
    }
    finally {
        if ($key) { $key.Dispose() }
        if ($base) { $base.Dispose() }
    }
}

function Get-MPCVRRegistryHint {
    # Where installers and the players themselves record their folders. An elevated process under
    # a different account has another HKCU; the HKLM entries and default folders still apply.
    $uninstall = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
    $appPaths = 'SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths'
    $sources = @(
        @{ Kind = 'K-Lite'; SubKey = "$uninstall\KLiteCodecPack_is1"; Name = 'InstallLocation' },
        @{ Kind = 'MPC-HC'; SubKey = "$uninstall\{2ACBF1FA-F5C3-4B19-A774-B22A31F231B9}_is1"; Name = 'InstallLocation' },
        @{ Kind = 'MPC-HC'; SubKey = "$uninstall\{2624B969-7135-4EB1-B0F6-2D8C397B45F7}_is1"; Name = 'InstallLocation' },
        @{ Kind = 'MPC-HC'; SubKey = "$appPaths\mpc-hc64.exe"; Name = '' },
        @{ Kind = 'MPC-HC'; SubKey = "$appPaths\mpc-hc.exe"; Name = '' },
        @{ Kind = 'MPC-BE'; SubKey = "$uninstall\{FE09AF6D-78B2-4093-B012-FCDAF78693CE}_is1"; Name = 'InstallLocation' },
        @{ Kind = 'MPC-BE'; SubKey = "$uninstall\{903D098F-DD50-4342-AD23-DA868FCA3126}_is1"; Name = 'InstallLocation' },
        @{ Kind = 'MPC-BE'; SubKey = "$appPaths\mpc-be64.exe"; Name = '' },
        @{ Kind = 'MPC-BE'; SubKey = "$appPaths\mpc-be.exe"; Name = '' },
        @{ Kind = 'MPC-BE'; SubKey = 'SOFTWARE\MPC-BE'; Name = 'ExePath' }
    )

    $hints = @()
    foreach ($source in $sources) {
        foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)) {
            $value = Read-RegistryValue -Hive LocalMachine -View $view -SubKey $source.SubKey -Name $source.Name
            if ($value) { $hints += [pscustomobject]@{ Kind = $source.Kind; Path = $value } }
        }
    }
    $lastRun = Read-RegistryValue -Hive CurrentUser -View Default -SubKey 'Software\MPC-HC\MPC-HC' -Name 'ExePath'
    if ($lastRun) { $hints += [pscustomobject]@{ Kind = 'MPC-HC'; Path = $lastRun } }
    return $hints
}

function ConvertTo-PlayerDirectory {
    param([string]$Path)

    try {
        $full = [IO.Path]::GetFullPath($Path.Trim().Trim('"'))
    }
    catch {
        return $null
    }
    if ($full.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)) { $full = [IO.Path]::GetDirectoryName($full) }
    if ([string]::IsNullOrEmpty($full)) { return $null }
    return $full.TrimEnd('\')
}

function Get-MPCVRPlayerTarget {
    [CmdletBinding()]
    param(
        # Parents of the default install folders. Tests point these at fixtures.
        [string[]]$ProgramRoot = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432),
        [object[]]$Hint = @(Get-MPCVRRegistryHint)
    )

    $roots = @($ProgramRoot | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $directories = @{ 'K-Lite' = @(); 'MPC-HC' = @(); 'MPC-BE' = @() }
    foreach ($item in @($Hint)) {
        $directory = ConvertTo-PlayerDirectory -Path ([string]$item.Path)
        if ($directory) { $directories[[string]$item.Kind] += $directory }
    }
    foreach ($root in $roots) {
        $directories['K-Lite'] += Join-Path $root 'K-Lite Codec Pack'
        $directories['MPC-HC'] += Join-Path $root 'MPC-HC'
        $directories['MPC-BE'] += Join-Path $root 'MPC-BE'
        $directories['MPC-BE'] += Join-Path $root 'MPC-BE x64'
    }

    $targets = New-Object Collections.ArrayList
    $seen = @{}
    $files = $script:RendererFiles
    $add = {
        param($Player, $Architecture, $Executable, $Destination, $LoadMode)
        $key = [IO.Path]::GetFullPath($Destination).ToLowerInvariant()
        if ($seen.ContainsKey($key)) { return }
        $seen[$key] = $true
        [void]$targets.Add([pscustomobject]@{
            Player = $Player
            Architecture = $Architecture
            Executable = $Executable
            FileName = $files[$Architecture]
            Destination = [IO.Path]::GetFullPath($Destination)
            LoadMode = $LoadMode
        })
    }

    # K-Lite comes first, so its copy of MPC-HC is never mistaken for a standalone one.
    foreach ($root in @($directories['K-Lite'] | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($layout in @(
            @{ Folder = 'MPC-HC64'; Exe = 'mpc-hc64.exe'; Architecture = 'x64' },
            @{ Folder = 'MPC-HC'; Exe = 'mpc-hc.exe'; Architecture = 'x86' }
        )) {
            $playerFolder = Join-Path $root $layout.Folder
            $executable = Join-Path $playerFolder $layout.Exe
            $destination = Join-Path $playerFolder "MPCVR\$($files[$layout.Architecture])"
            if ((Test-Path -LiteralPath $executable -PathType Leaf) -or (Test-Path -LiteralPath $destination -PathType Leaf)) {
                & $add 'K-Lite Codec Pack (MPC-HC)' $layout.Architecture $executable $destination 'PlayerFolder'
            }
        }
        # The pack's shared filters, used by whatever else plays through DirectShow. Only replaced, never added.
        foreach ($filter in @(
            @{ Path = 'Filters\MPCVR\MpcVideoRenderer.ax'; Architecture = 'x86' },
            @{ Path = 'Filters64\MPCVR\MpcVideoRenderer64.ax'; Architecture = 'x64' }
        )) {
            $destination = Join-Path $root $filter.Path
            if (Test-Path -LiteralPath $destination -PathType Leaf) {
                & $add 'K-Lite Codec Pack (filter)' $filter.Architecture $null $destination 'SharedFilter'
            }
        }
    }

    foreach ($player in @(
        @{ Name = 'MPC-HC'; Subfolder = 'MPCVR'; LoadMode = 'PlayerFolder' },
        @{ Name = 'MPC-BE'; Subfolder = 'Filters'; LoadMode = 'Registration' }
    )) {
        foreach ($directory in @($directories[$player.Name] | Select-Object -Unique)) {
            foreach ($layout in $script:PlayerExecutables[$player.Name]) {
                $executable = Join-Path $directory $layout.Exe
                if (Test-Path -LiteralPath $executable -PathType Leaf) {
                    $destination = Join-Path $directory "$($player.Subfolder)\$($files[$layout.Architecture])"
                    & $add $player.Name $layout.Architecture $executable $destination $player.LoadMode
                }
            }
        }
    }

    return @($targets)
}

function Get-MPCVRRequiredPlayerTarget {
    [CmdletBinding()]
    param(
        [string[]]$ProgramRoot = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432),
        [object[]]$Hint = @(Get-MPCVRRegistryHint)
    )

    $targets = @(Get-MPCVRPlayerTarget -ProgramRoot $ProgramRoot -Hint $Hint)
    if ($targets.Count -eq 0) {
        throw 'No supported player was found. Install MPC-HC, MPC-BE, or K-Lite Codec Pack with MPC-HC, then run this again. A portable player copy has to be updated by hand from MPCVR-Portable.zip.'
    }
    return $targets
}

function Invoke-Regsvr32 {
    param([string]$Architecture, [string[]]$Arguments)

    $folder = if ($Architecture -eq 'x86' -and [Environment]::Is64BitOperatingSystem) { 'SysWOW64' }
        elseif ($Architecture -eq 'x64' -and -not [Environment]::Is64BitProcess) { 'Sysnative' }
        else { 'System32' }
    $process = Start-Process -FilePath (Join-Path $env:SystemRoot "$folder\regsvr32.exe") -ArgumentList $Arguments -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) { throw "regsvr32 $($Arguments -join ' ') failed with exit code $($process.ExitCode)." }
}

function Read-RendererRegistration {
    # Matches MPC-BE's own check: the filter instance and an in-process server must both exist.
    param([string]$Architecture)

    $view = if ($Architecture -eq 'x86') { [Microsoft.Win32.RegistryView]::Registry32 } else { [Microsoft.Win32.RegistryView]::Registry64 }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::ClassesRoot, $view)
    try {
        $instance = $base.OpenSubKey("CLSID\$($script:FilterCategory)\Instance\$($script:RendererClsid)")
        if ($null -eq $instance) { return $null }
        $instance.Dispose()
        $server = $base.OpenSubKey("CLSID\$($script:RendererClsid)\InprocServer32")
        if ($null -eq $server) { return $null }
        try {
            $value = [string]$server.GetValue('')
            if ([string]::IsNullOrWhiteSpace($value)) { return $null }
            return $value.Trim().Trim('"')
        }
        finally { $server.Dispose() }
    }
    finally { $base.Dispose() }
}

function New-MPCVRRegistrationProvider {
    # Replaceable so the transaction can be tested without changing the machine's registration.
    return @{
        Read = { param($Architecture) Read-RendererRegistration -Architecture $Architecture }
        Register = { param($Architecture, $Path) Invoke-Regsvr32 -Architecture $Architecture -Arguments @('/s', ('"{0}"' -f $Path)) }
        Unregister = { param($Architecture, $Path) Invoke-Regsvr32 -Architecture $Architecture -Arguments @('/u', '/s', ('"{0}"' -f $Path)) }
    }
}

function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-SamePath {
    param([string]$Left, [string]$Right)
    try {
        return [IO.Path]::GetFullPath($Left).TrimEnd('\') -ieq [IO.Path]::GetFullPath($Right).TrimEnd('\')
    }
    catch {
        return $false
    }
}

function Install-MPCVRPlayerRenderer {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][hashtable]$Sources,
        [Parameter(Mandatory = $true)][object[]]$Targets,
        [hashtable]$Registration = (New-MPCVRRegistrationProvider)
    )

    if ($Targets.Count -eq 0) { throw 'No renderer targets were supplied.' }

    $changes = New-Object Collections.ArrayList
    $createdDirectories = New-Object Collections.ArrayList
    $registrations = New-Object Collections.ArrayList
    $expected = @{}

    try {
        foreach ($fileName in @($Targets | ForEach-Object { $_.FileName } | Select-Object -Unique)) {
            if (-not $Sources.ContainsKey($fileName)) { throw "The renderer package does not contain $fileName." }
            if (-not (Test-Path -LiteralPath ([string]$Sources[$fileName]) -PathType Leaf)) { throw "Renderer source was not found: $($Sources[$fileName])" }
            $expected[$fileName] = Get-FileSha256 -Path ([string]$Sources[$fileName])
        }

        foreach ($target in $Targets) {
            $destination = [string]$target.Destination
            $directory = [IO.Path]::GetDirectoryName($destination)
            $missing = @()
            $walk = $directory
            while ($walk -and -not (Test-Path -LiteralPath $walk -PathType Container)) {
                $missing += $walk
                $walk = [IO.Path]::GetDirectoryName($walk)
            }
            if ($missing.Count) {
                [void](New-Item -ItemType Directory -Path $directory -Force)
                [void]$createdDirectories.AddRange([object[]]$missing)
            }

            $entry = [pscustomobject]@{ Destination = $destination; Backup = $null; OriginalSha256 = $null }
            if (Test-Path -LiteralPath $destination -PathType Leaf) {
                $entry.Backup = "$destination.mpcvr-backup-$([guid]::NewGuid().ToString('N'))"
                Copy-Item -LiteralPath $destination -Destination $entry.Backup -Force
                $entry.OriginalSha256 = Get-FileSha256 -Path $entry.Backup
            }
            [void]$changes.Add($entry)

            Copy-Item -LiteralPath ([string]$Sources[$target.FileName]) -Destination $destination -Force
            if ((Get-FileSha256 -Path $destination) -ne $expected[$target.FileName]) {
                throw "Verification failed after copying $($target.FileName) to $destination."
            }
        }

        # MPC-BE only loads what is registered. A registered file that is already this build is
        # left where it is, so K-Lite or another owner keeps the registration; anything else would
        # leave MPC-BE loading the wrong renderer, so its own copy takes over.
        foreach ($target in @($Targets | Where-Object { $_.LoadMode -eq 'Registration' } | Group-Object Architecture | ForEach-Object { $_.Group[0] })) {
            $current = & $Registration.Read $target.Architecture
            if ($current -and (Test-Path -LiteralPath $current -PathType Leaf) -and ((Get-FileSha256 -Path $current) -eq $expected[$target.FileName])) {
                continue
            }

            [void]$registrations.Add([pscustomobject]@{ Architecture = $target.Architecture; Previous = $current; Applied = $target.Destination })
            & $Registration.Register $target.Architecture $target.Destination
            $registered = & $Registration.Read $target.Architecture
            if (-not $registered -or -not (Test-SamePath -Left $registered -Right $target.Destination)) {
                throw "Registering $($target.Destination) left the $($target.Architecture) renderer registered as '$registered'."
            }
        }

        # What each player will load, checked against the verified package.
        $installed = @()
        foreach ($target in $Targets) {
            $loads = $target.Destination
            if ($target.LoadMode -eq 'Registration') { $loads = & $Registration.Read $target.Architecture }
            if (-not $loads -or -not (Test-Path -LiteralPath $loads -PathType Leaf) -or (Get-FileSha256 -Path $loads) -ne $expected[$target.FileName]) {
                throw "$($target.Player) ($($target.Architecture)) would not load this build; it would load '$loads'."
            }
            $item = Get-Item -LiteralPath $target.Destination
            $installed += [pscustomobject]@{
                Player = $target.Player
                Architecture = $target.Architecture
                File = $item.Name
                Version = $item.VersionInfo.FileVersion
                Path = $item.FullName
                LoadsFrom = $loads
                Sha256 = $expected[$target.FileName]
            }
        }

        foreach ($entry in $changes) {
            if ($entry.Backup) { Remove-Item -LiteralPath $entry.Backup -Force -ErrorAction SilentlyContinue }
        }
        $changes.Clear()
        return $installed
    }
    catch {
        $installError = $_
        $rollbackErrors = @()

        # Registration first: unregistering needs the new file, and re-registering a previous
        # path only records it, so its contents can be restored afterwards.
        for ($index = $registrations.Count - 1; $index -ge 0; $index--) {
            $entry = $registrations[$index]
            try {
                if ($entry.Previous -and (Test-Path -LiteralPath $entry.Previous -PathType Leaf)) {
                    & $Registration.Register $entry.Architecture $entry.Previous
                }
                else {
                    & $Registration.Unregister $entry.Architecture $entry.Applied
                }
            }
            catch {
                $rollbackErrors += "$($entry.Architecture) registration: $($_.Exception.Message)"
            }
        }

        for ($index = $changes.Count - 1; $index -ge 0; $index--) {
            $entry = $changes[$index]
            try {
                if ($entry.Backup) {
                    Copy-Item -LiteralPath $entry.Backup -Destination $entry.Destination -Force
                    if ((Get-FileSha256 -Path $entry.Destination) -ne $entry.OriginalSha256) {
                        throw 'The restored hash does not match the original file.'
                    }
                }
                elseif (Test-Path -LiteralPath $entry.Destination -PathType Leaf) {
                    Remove-Item -LiteralPath $entry.Destination -Force
                }
            }
            catch {
                $rollbackErrors += "$($entry.Destination): $($_.Exception.Message)"
            }
        }

        foreach ($directory in @($createdDirectories | Sort-Object Length -Descending)) {
            Remove-Item -LiteralPath $directory -Force -ErrorAction SilentlyContinue
        }

        if ($rollbackErrors.Count -ne 0) {
            throw "Renderer installation failed: $($installError.Exception.Message) Rollback also failed: $($rollbackErrors -join '; ')"
        }
        throw $installError
    }
    finally {
        foreach ($entry in $changes) {
            if ($entry.Backup) { Remove-Item -LiteralPath $entry.Backup -Force -ErrorAction SilentlyContinue }
        }
    }
}

Export-ModuleMember -Function 'Get-MPCVRPlayerProcessName', 'Get-MPCVRRegistryHint', 'Get-MPCVRPlayerTarget', 'Get-MPCVRRequiredPlayerTarget', 'New-MPCVRRegistrationProvider', 'Install-MPCVRPlayerRenderer'
