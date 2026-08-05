#requires -Version 5.1

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$guiPath = Join-Path $repoRoot 'tools\UnifiedSetup\Start-MpcvrUnifiedSetup.ps1'
$profilesPath = Join-Path $repoRoot 'tools\UnifiedSetup\MpcvrSetup.Profiles.psm1'

function Replace-RequiredText {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Old,
        [Parameter(Mandatory)][string]$New
    )

    $content = Get-Content -LiteralPath $Path -Raw
    if (-not $content.Contains($Old)) {
        throw "Required patch anchor was not found in $Path.`n--- anchor ---`n$Old"
    }
    Set-Content -LiteralPath $Path -Value $content.Replace($Old, $New) -Encoding UTF8
}

Replace-RequiredText -Path $profilesPath -Old @'
function Get-MpcvrProfileRoot {
    param(
        [string]$ProfileRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup\profiles')
    )

    if ([string]::IsNullOrWhiteSpace($ProfileRoot)) {
        throw 'A profile root is required.'
    }
    return [IO.Path]::GetFullPath($ProfileRoot)
}
'@ -New @'
function Get-MpcvrProfileRoot {
    param(
        [string]$ProfileRoot
    )

    if ([string]::IsNullOrWhiteSpace($ProfileRoot)) {
        $ProfileRoot = Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup\profiles'
    }
    return [IO.Path]::GetFullPath($ProfileRoot)
}
'@

Replace-RequiredText -Path $guiPath -Old @'
function New-Button {
    param([string]$Text, [int]$X, [int]$Y, [int]$Width = 145, [int]$Height = 32)
    $control = New-Object System.Windows.Forms.Button
    $control.Text = $Text
    $control.Location = New-Object System.Drawing.Point($X, $Y)
    $control.Size = New-Object System.Drawing.Size($Width, $Height)
    return $control
}

if ($ValidateOnly) {
    $testForm = New-Object System.Windows.Forms.Form
    $testForm.Text = 'MPCVR Unified Setup Validation'
    $testForm.Controls.Add((New-Label -Text 'Validation' -X 10 -Y 10))
    $testForm.Dispose()
    [void](Get-PowerShellExecutable)
    Write-Host 'MPCVR Unified Setup GUI validation passed.' -ForegroundColor Green
    exit 0
}
'@ -New @'
function New-Button {
    param([string]$Text, [int]$X, [int]$Y, [int]$Width = 145, [int]$Height = 32)
    $control = New-Object System.Windows.Forms.Button
    $control.Text = $Text
    $control.Location = New-Object System.Drawing.Point($X, $Y)
    $control.Size = New-Object System.Drawing.Size($Width, $Height)
    return $control
}

function Get-MpcvrOptionalProperty {
    param(
        [object]$InputObject,
        [Parameter(Mandatory)][string]$Name,
        $DefaultValue = $null
    )

    if ($null -eq $InputObject) {
        return $DefaultValue
    }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        return $DefaultValue
    }
    return $property.Value
}

function ConvertTo-MpcvrSystemStatusLines {
    param(
        [Parameter(Mandatory)]
        [object]$Profile
    )

    $lines = @()
    foreach ($gpu in @(Get-MpcvrOptionalProperty -InputObject $Profile -Name 'Gpus' -DefaultValue @())) {
        $name = [string](Get-MpcvrOptionalProperty -InputObject $gpu -Name 'Name' -DefaultValue 'Unknown NVIDIA GPU')
        $driver = [string](Get-MpcvrOptionalProperty -InputObject $gpu -Name 'DriverVersion' -DefaultValue 'unknown')
        $memoryMiB = [double](Get-MpcvrOptionalProperty -InputObject $gpu -Name 'MemoryMiB' -DefaultValue 0)
        $vram = if ($memoryMiB -gt 0) { '{0:N1} GB' -f ($memoryMiB / 1024.0) } else { 'unknown VRAM' }
        $width = [int](Get-MpcvrOptionalProperty -InputObject $gpu -Name 'CurrentWidth' -DefaultValue 0)
        $height = [int](Get-MpcvrOptionalProperty -InputObject $gpu -Name 'CurrentHeight' -DefaultValue 0)
        $refresh = [int](Get-MpcvrOptionalProperty -InputObject $gpu -Name 'CurrentRefreshHz' -DefaultValue 0)
        $output = if ($width -gt 0 -and $height -gt 0) {
            if ($refresh -gt 0) { "$width`x$height @ $refresh Hz" } else { "$width`x$height" }
        }
        else {
            'output unknown'
        }
        $lines += "GPU: $name | driver $driver | $vram | $output"
    }

    foreach ($display in @(Get-MpcvrOptionalProperty -InputObject $Profile -Name 'Displays' -DefaultValue @())) {
        $name = [string](Get-MpcvrOptionalProperty -InputObject $display -Name 'Name' -DefaultValue 'Unknown display')
        $width = [int](Get-MpcvrOptionalProperty -InputObject $display -Name 'ScreenWidth' -DefaultValue 0)
        $height = [int](Get-MpcvrOptionalProperty -InputObject $display -Name 'ScreenHeight' -DefaultValue 0)
        $status = [string](Get-MpcvrOptionalProperty -InputObject $display -Name 'Status' -DefaultValue 'unknown')
        $size = if ($width -gt 0 -and $height -gt 0) { "$width`x$height" } else { 'size unknown' }
        $lines += "Display: $name | $size | status $status"
    }

    $runtimes = Get-MpcvrOptionalProperty -InputObject $Profile -Name 'Runtimes'
    $maxine = Get-MpcvrOptionalProperty -InputObject $runtimes -Name 'Maxine'
    $fruc = Get-MpcvrOptionalProperty -InputObject $runtimes -Name 'NvOFFRUC'
    $lines += 'Maxine: {0} | {1}' -f `
        [bool](Get-MpcvrOptionalProperty -InputObject $maxine -Name 'Installed' -DefaultValue $false), `
        [string](Get-MpcvrOptionalProperty -InputObject $maxine -Name 'Path' -DefaultValue '')
    $lines += 'NvOFFRUC: {0} | {1}' -f `
        [bool](Get-MpcvrOptionalProperty -InputObject $fruc -Name 'Installed' -DefaultValue $false), `
        [string](Get-MpcvrOptionalProperty -InputObject $fruc -Name 'Path' -DefaultValue '')

    foreach ($player in @(Get-MpcvrOptionalProperty -InputObject $Profile -Name 'Players' -DefaultValue @())) {
        $lines += '{0}: folder={1}, renderer={2}, version={3}' -f `
            [string](Get-MpcvrOptionalProperty -InputObject $player -Name 'Name' -DefaultValue 'Player'), `
            [bool](Get-MpcvrOptionalProperty -InputObject $player -Name 'DirectoryExists' -DefaultValue $false), `
            [bool](Get-MpcvrOptionalProperty -InputObject $player -Name 'RendererExists' -DefaultValue $false), `
            [string](Get-MpcvrOptionalProperty -InputObject $player -Name 'RendererVersion' -DefaultValue '')
    }

    if ($lines.Count -eq 0) {
        $lines += 'No supported NVIDIA GPU, display, runtime, or player information was detected.'
    }
    return $lines
}

if ($ValidateOnly) {
    $testForm = New-Object System.Windows.Forms.Form
    $testForm.Text = 'MPCVR Unified Setup Validation'
    $testForm.Controls.Add((New-Label -Text 'Validation' -X 10 -Y 10))
    $testForm.Dispose()
    [void](Get-PowerShellExecutable)

    $synthetic = [pscustomobject]@{
        Gpus = @([pscustomobject]@{
            Name = 'Synthetic GPU'
            DriverVersion = '1.2.3'
            MemoryMiB = 8192
            CurrentWidth = 1920
            CurrentHeight = 1080
            CurrentRefreshHz = 240
        })
        Displays = @([pscustomobject]@{
            Name = 'Synthetic Display'
            ScreenWidth = 1920
            ScreenHeight = 1080
            Status = 'OK'
        })
        Runtimes = [pscustomobject]@{
            Maxine = [pscustomobject]@{ Installed = $true; Path = 'C:\Maxine' }
            NvOFFRUC = [pscustomobject]@{ Installed = $true; Path = 'C:\NvOFFRUC' }
        }
        Players = @([pscustomobject]@{
            Name = 'Synthetic MPC-HC'
            DirectoryExists = $true
            RendererExists = $true
            RendererVersion = '1.0.0'
        })
    }
    $statusLines = @(ConvertTo-MpcvrSystemStatusLines -Profile $synthetic)
    if ($statusLines.Count -lt 5 -or $statusLines[0] -notmatch '8\.0 GB') {
        throw 'GUI inventory formatting validation failed.'
    }

    $defaultProfileRoot = Get-MpcvrProfileRoot
    if ([string]::IsNullOrWhiteSpace($defaultProfileRoot)) {
        throw 'Default profile-root resolution failed.'
    }
    [void]@(Get-MpcvrProfiles -ProfileRoot $defaultProfileRoot)

    Write-Host 'MPCVR Unified Setup GUI validation passed.' -ForegroundColor Green
    exit 0
}
'@

Replace-RequiredText -Path $guiPath -Old "New-Button -Text 'Browseâ€¦'" -New "New-Button -Text 'Browse...'"
Replace-RequiredText -Path $guiPath -Old '30â†’60' -New '30 -> 60'
Replace-RequiredText -Path $guiPath -Old '60â†’120' -New '60 -> 120'

Replace-RequiredText -Path $guiPath -Old @'
function Refresh-SystemStatus {
    try {
        $profile = Get-MpcvrSystemProfile
        $lines = @()
        foreach ($gpu in @($profile.Gpus)) {
            $vram = if ($null -ne $gpu.AdapterRamGB) { '{0:N1} GB' -f [double]$gpu.AdapterRamGB } else { 'unknown VRAM' }
            $lines += "GPU: $($gpu.Name) | driver $($gpu.DriverVersion) | $vram | $($gpu.CurrentWidth)x$($gpu.CurrentHeight) @ $($gpu.CurrentRefreshHz) Hz"
        }
        foreach ($display in @($profile.Displays)) {
            $lines += "Display: $($display.Name) | $($display.Width)x$($display.Height) @ $($display.RefreshHz) Hz"
        }
        $lines += "Maxine: $($profile.Runtimes.Maxine.Installed) | $($profile.Runtimes.Maxine.Path)"
        $lines += "NvOFFRUC: $($profile.Runtimes.NvOFFRUC.Installed) | $($profile.Runtimes.NvOFFRUC.Path)"
        foreach ($player in @($profile.Players)) {
            $lines += "$($player.Name): folder=$($player.DirectoryExists), renderer=$($player.RendererExists), version=$($player.RendererVersion)"
        }
        $statusText.Lines = $lines
        Set-Activity -Text 'System status refreshed.' -Color ([System.Drawing.Color]::DarkGreen)
    }
    catch {
        $statusText.Text = $_.Exception.Message
        Set-Activity -Text 'Status refresh failed.' -Color ([System.Drawing.Color]::DarkRed)
    }
}

function Refresh-Profiles {
    $profileCombo.Items.Clear()
    foreach ($stored in @(Get-MpcvrProfiles)) {
        if ($stored.Valid) {
            [void]$profileCombo.Items.Add($stored.Name)
        }
    }
    if ($profileCombo.Items.Count -gt 0) {
        $profileCombo.SelectedIndex = 0
    }
}
'@ -New @'
function Refresh-SystemStatus {
    try {
        $profile = Get-MpcvrSystemProfile
        $statusText.Lines = @(ConvertTo-MpcvrSystemStatusLines -Profile $profile)
        Set-Activity -Text 'System status refreshed.' -Color ([System.Drawing.Color]::DarkGreen)
    }
    catch {
        $statusText.Text = $_.Exception.Message
        Set-Activity -Text 'Status refresh failed.' -Color ([System.Drawing.Color]::DarkRed)
    }
}

function Refresh-Profiles {
    try {
        $profileCombo.Items.Clear()
        $root = Get-MpcvrProfileRoot
        foreach ($stored in @(Get-MpcvrProfiles -ProfileRoot $root)) {
            if ($stored.Valid) {
                [void]$profileCombo.Items.Add($stored.Name)
            }
        }
        if ($profileCombo.Items.Count -gt 0) {
            $profileCombo.SelectedIndex = 0
        }
    }
    catch {
        Set-Activity -Text "Profile refresh failed: $($_.Exception.Message)" -Color ([System.Drawing.Color]::DarkRed)
    }
}
'@

Write-Host 'Unified setup first-run GUI fixes applied.' -ForegroundColor Green
