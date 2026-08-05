#requires -Version 5.1

[CmdletBinding()]
param(
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$commonModule = Join-Path $PSScriptRoot 'MpcvrSetup.Common.psm1'
$profilesModule = Join-Path $PSScriptRoot 'MpcvrSetup.Profiles.psm1'
$firstRunScript = Join-Path $PSScriptRoot 'Invoke-MpcvrFirstRun.ps1'
$installerScript = Join-Path $PSScriptRoot 'Install-MpcvrUnified.ps1'
$autoTuneScript = Join-Path $PSScriptRoot 'Invoke-MpcvrAutoTune.ps1'
$applyScript = Join-Path $PSScriptRoot 'Set-MpcvrRendererProfile.ps1'
$profileManagerScript = Join-Path $PSScriptRoot 'Manage-MpcvrProfiles.ps1'
$rollbackScript = Join-Path $PSScriptRoot 'Restore-MpcvrUnifiedBackup.ps1'
$telemetryScript = Join-Path $PSScriptRoot 'Get-MpcvrRendererTelemetry.ps1'
foreach ($path in @(
    $commonModule, $profilesModule, $firstRunScript, $installerScript,
    $autoTuneScript, $applyScript, $profileManagerScript, $rollbackScript,
    $telemetryScript
)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Unified setup GUI dependency is missing: $path"
    }
}

Import-Module -Name $commonModule -Force
Import-Module -Name $profilesModule -Force

function Get-PowerShellExecutable {
    $pwsh = Get-Command 'pwsh.exe' -ErrorAction SilentlyContinue
    if ($null -ne $pwsh) { return $pwsh.Source }
    $powershell = Get-Command 'powershell.exe' -ErrorAction SilentlyContinue
    if ($null -ne $powershell) { return $powershell.Source }
    throw 'Neither PowerShell 7 nor Windows PowerShell 5.1 was found.'
}

function Quote-Argument {
    param([string]$Value)
    return '"{0}"' -f $Value.Replace('"', '\"')
}

function Start-MpcvrTool {
    param(
        [Parameter(Mandatory)]
        [string]$ScriptPath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)]
        [string]$Description
    )

    $parts = @(
        '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (Quote-Argument -Value $ScriptPath)
    ) + @($Arguments)
    [void](Start-Process `
        -FilePath (Get-PowerShellExecutable) `
        -ArgumentList ($parts -join ' '))
    return "$Description started in a separate window."
}

function Select-MpcvrFile {
    param(
        [Parameter(Mandatory)]
        [string]$Title,
        [Parameter(Mandatory)]
        [string]$Filter
    )

    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = $Title
    $dialog.Filter = $Filter
    $dialog.CheckFileExists = $true
    try {
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            return $dialog.FileName
        }
        return $null
    }
    finally {
        $dialog.Dispose()
    }
}

function New-Label {
    param([string]$Text, [int]$X, [int]$Y, [int]$Width = 150, [int]$Height = 22)
    $control = New-Object System.Windows.Forms.Label
    $control.Text = $Text
    $control.Location = New-Object System.Drawing.Point($X, $Y)
    $control.Size = New-Object System.Drawing.Size($Width, $Height)
    return $control
}

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

$form = New-Object System.Windows.Forms.Form
$form.Text = 'MPCVR Unified Setup'
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object System.Drawing.Size(980, 760)
$form.MinimumSize = New-Object System.Drawing.Size(996, 799)
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)

$title = New-Label -Text 'MPC Video Renderer + NVIDIA Maxine + Frame Interpolation' -X 18 -Y 14 -Width 760 -Height 30
$title.Font = New-Object System.Drawing.Font('Segoe UI Semibold', 16)
$form.Controls.Add($title)

$subtitle = New-Label -Text 'Automatic for friends. Guided when you want choices. Advanced when you want everything.' -X 20 -Y 48 -Width 850 -Height 24
$subtitle.ForeColor = [System.Drawing.Color]::DimGray
$form.Controls.Add($subtitle)

$statusGroup = New-Object System.Windows.Forms.GroupBox
$statusGroup.Text = 'Detected system'
$statusGroup.Location = New-Object System.Drawing.Point(18, 80)
$statusGroup.Size = New-Object System.Drawing.Size(944, 184)
$form.Controls.Add($statusGroup)

$statusText = New-Object System.Windows.Forms.TextBox
$statusText.Location = New-Object System.Drawing.Point(14, 24)
$statusText.Size = New-Object System.Drawing.Size(805, 145)
$statusText.Multiline = $true
$statusText.ReadOnly = $true
$statusText.ScrollBars = 'Vertical'
$statusText.BackColor = [System.Drawing.SystemColors]::Window
$statusGroup.Controls.Add($statusText)

$refreshStatusButton = New-Button -Text 'Refresh status' -X 831 -Y 25 -Width 98
$statusGroup.Controls.Add($refreshStatusButton)
$telemetryButton = New-Button -Text 'Live telemetry' -X 831 -Y 65 -Width 98
$statusGroup.Controls.Add($telemetryButton)
$rollbackButton = New-Button -Text 'Rollback' -X 831 -Y 105 -Width 98
$statusGroup.Controls.Add($rollbackButton)

$inputsGroup = New-Object System.Windows.Forms.GroupBox
$inputsGroup.Text = 'Setup inputs'
$inputsGroup.Location = New-Object System.Drawing.Point(18, 274)
$inputsGroup.Size = New-Object System.Drawing.Size(944, 112)
$form.Controls.Add($inputsGroup)

$inputsGroup.Controls.Add((New-Label -Text 'Optical Flow SDK (optional)' -X 14 -Y 27 -Width 175))
$sdkText = New-Object System.Windows.Forms.TextBox
$sdkText.Location = New-Object System.Drawing.Point(194, 24)
$sdkText.Size = New-Object System.Drawing.Size(645, 24)
$inputsGroup.Controls.Add($sdkText)
$sdkBrowse = New-Button -Text 'Browse...' -X 850 -Y 21 -Width 78 -Height 28
$inputsGroup.Controls.Add($sdkBrowse)
$sdkToolTip = New-Object System.Windows.Forms.ToolTip
$sdkToolTip.SetToolTip($sdkText, 'Leave blank. Setup will open NVIDIA''s official login/download page, detect the completed SDK ZIP, validate it, and continue automatically.')
$sdkToolTip.SetToolTip($sdkBrowse, 'Optional manual fallback for an SDK ZIP you already downloaded.')

$inputsGroup.Controls.Add((New-Label -Text 'Calibration video' -X 14 -Y 67 -Width 145))
$mediaText = New-Object System.Windows.Forms.TextBox
$mediaText.Location = New-Object System.Drawing.Point(164, 64)
$mediaText.Size = New-Object System.Drawing.Size(675, 24)
$inputsGroup.Controls.Add($mediaText)
$mediaBrowse = New-Button -Text 'Browse...' -X 850 -Y 61 -Width 78 -Height 28
$inputsGroup.Controls.Add($mediaBrowse)

$tabs = New-Object System.Windows.Forms.TabControl
$tabs.Location = New-Object System.Drawing.Point(18, 396)
$tabs.Size = New-Object System.Drawing.Size(944, 278)
$form.Controls.Add($tabs)

$automaticTab = New-Object System.Windows.Forms.TabPage
$automaticTab.Text = 'Automatic'
$tabs.TabPages.Add($automaticTab)
$automaticDescription = New-Label -Text 'Install or update all components, acquiring the Optical Flow SDK through NVIDIA''s official page when needed, then test ranked settings until one passes. Original settings are restored when no candidate passes.' -X 18 -Y 20 -Width 870 -Height 52
$automaticTab.Controls.Add($automaticDescription)
$automaticTab.Controls.Add((New-Label -Text 'Profile name (optional)' -X 18 -Y 78 -Width 155))
$automaticProfileName = New-Object System.Windows.Forms.TextBox
$automaticProfileName.Location = New-Object System.Drawing.Point(178, 75)
$automaticProfileName.Size = New-Object System.Drawing.Size(430, 24)
$automaticTab.Controls.Add($automaticProfileName)
$automaticLock = New-Object System.Windows.Forms.CheckBox
$automaticLock.Text = 'Lock the passing profile'
$automaticLock.Location = New-Object System.Drawing.Point(625, 76)
$automaticLock.Size = New-Object System.Drawing.Size(190, 24)
$automaticTab.Controls.Add($automaticLock)
$firstRunButton = New-Button -Text 'Install + auto-tune' -X 18 -Y 122 -Width 180 -Height 42
$firstRunButton.Font = New-Object System.Drawing.Font('Segoe UI Semibold', 10)
$automaticTab.Controls.Add($firstRunButton)
$installOnlyButton = New-Button -Text 'Install / update only' -X 212 -Y 122 -Width 170 -Height 42
$automaticTab.Controls.Add($installOnlyButton)
$autoTuneOnlyButton = New-Button -Text 'Auto-tune only' -X 396 -Y 122 -Width 150 -Height 42
$automaticTab.Controls.Add($autoTuneOnlyButton)
$automaticNote = New-Label -Text 'Use a representative 30 fps video to build a 30 -> 60 profile and a representative 60 fps video to build a separate 60 -> 120 profile.' -X 18 -Y 184 -Width 860 -Height 42
$automaticNote.ForeColor = [System.Drawing.Color]::DimGray
$automaticTab.Controls.Add($automaticNote)

$guidedTab = New-Object System.Windows.Forms.TabPage
$guidedTab.Text = 'Guided'
$tabs.TabPages.Add($guidedTab)
$guidedTab.Controls.Add((New-Label -Text 'Choose what the recommendation engine should preserve when the measured combination is too expensive.' -X 18 -Y 20 -Width 850 -Height 42))
$guidedTab.Controls.Add((New-Label -Text 'Priority' -X 18 -Y 78 -Width 100))
$priorityCombo = New-Object System.Windows.Forms.ComboBox
$priorityCombo.Location = New-Object System.Drawing.Point(120, 75)
$priorityCombo.Size = New-Object System.Drawing.Size(210, 24)
$priorityCombo.DropDownStyle = 'DropDownList'
[void]$priorityCombo.Items.AddRange(@('Balanced', 'Smoothness', 'Quality'))
$priorityCombo.SelectedIndex = 0
$guidedTab.Controls.Add($priorityCombo)
$guidedRunButton = New-Button -Text 'Install + guided auto-tune' -X 18 -Y 122 -Width 210 -Height 42
$guidedTab.Controls.Add($guidedRunButton)
$guidedTuneButton = New-Button -Text 'Guided auto-tune only' -X 242 -Y 122 -Width 190 -Height 42
$guidedTab.Controls.Add($guidedTuneButton)
$guidedExplanation = New-Label -Text 'Balanced begins with low-impact reductions. Smoothness preserves interpolation. Quality preserves Maxine enhancement.' -X 18 -Y 184 -Width 850 -Height 42
$guidedExplanation.ForeColor = [System.Drawing.Color]::DimGray
$guidedTab.Controls.Add($guidedExplanation)

$advancedTab = New-Object System.Windows.Forms.TabPage
$advancedTab.Text = 'Advanced'
$tabs.TabPages.Add($advancedTab)
$advancedTab.Controls.Add((New-Label -Text 'Stored profile' -X 18 -Y 24 -Width 110))
$profileCombo = New-Object System.Windows.Forms.ComboBox
$profileCombo.Location = New-Object System.Drawing.Point(132, 21)
$profileCombo.Size = New-Object System.Drawing.Size(510, 24)
$profileCombo.DropDownStyle = 'DropDownList'
$advancedTab.Controls.Add($profileCombo)
$refreshProfilesButton = New-Button -Text 'Refresh' -X 654 -Y 18 -Width 90 -Height 30
$advancedTab.Controls.Add($refreshProfilesButton)
$previewProfileButton = New-Button -Text 'Preview changes' -X 18 -Y 70 -Width 150 -Height 38
$advancedTab.Controls.Add($previewProfileButton)
$applyProfileButton = New-Button -Text 'Apply profile' -X 180 -Y 70 -Width 140 -Height 38
$advancedTab.Controls.Add($applyProfileButton)
$currentSettingsButton = New-Button -Text 'Show current settings' -X 332 -Y 70 -Width 170 -Height 38
$advancedTab.Controls.Add($currentSettingsButton)
$manageProfilesButton = New-Button -Text 'Profile manager' -X 514 -Y 70 -Width 140 -Height 38
$advancedTab.Controls.Add($manageProfilesButton)
$openProfilesButton = New-Button -Text 'Open profile folder' -X 666 -Y 70 -Width 150 -Height 38
$advancedTab.Controls.Add($openProfilesButton)
$advancedText = New-Label -Text 'Advanced profiles can carry the exact MPCVR DWORD settings and arbitrary future fields. Profile locks prevent automatic calibration from overwriting manual work. The native MPCVR property pages remain available for direct renderer editing.' -X 18 -Y 132 -Width 860 -Height 70
$advancedText.ForeColor = [System.Drawing.Color]::DimGray
$advancedTab.Controls.Add($advancedText)

$activityLabel = New-Label -Text 'Ready.' -X 20 -Y 689 -Width 800 -Height 28
$activityLabel.ForeColor = [System.Drawing.Color]::DarkSlateGray
$form.Controls.Add($activityLabel)
$closeButton = New-Button -Text 'Close' -X 862 -Y 684 -Width 98 -Height 34
$form.Controls.Add($closeButton)

function Set-Activity {
    param([string]$Text, [System.Drawing.Color]$Color = [System.Drawing.Color]::DarkSlateGray)
    $activityLabel.Text = $Text
    $activityLabel.ForeColor = $Color
}

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

function Get-CommonArguments {
    param([string]$Mode, [string]$Priority)
    $arguments = @('-Mode', $Mode, '-Priority', $Priority, '-NoPause')
    if (-not [string]::IsNullOrWhiteSpace($sdkText.Text)) {
        $arguments += @('-NvOffrucSdkPath', (Quote-Argument -Value $sdkText.Text))
    }
    if (-not [string]::IsNullOrWhiteSpace($mediaText.Text)) {
        $arguments += @('-MediaPath', (Quote-Argument -Value $mediaText.Text))
    }
    if (-not [string]::IsNullOrWhiteSpace($automaticProfileName.Text)) {
        $arguments += @('-ProfileName', (Quote-Argument -Value $automaticProfileName.Text))
    }
    if ($automaticLock.Checked) {
        $arguments += '-LockProfile'
    }
    return $arguments
}

$sdkBrowse.Add_Click({
    $selected = Select-MpcvrFile `
        -Title 'Select Optical_Flow_SDK_5.0.7.zip' `
        -Filter 'NVIDIA Optical Flow SDK ZIP (*.zip)|*.zip|All files (*.*)|*.*'
    if ($null -ne $selected) { $sdkText.Text = $selected }
})

$mediaBrowse.Add_Click({
    $selected = Select-MpcvrFile `
        -Title 'Select a representative calibration video' `
        -Filter 'Video files|*.mkv;*.mp4;*.webm;*.avi;*.mov;*.m2ts;*.ts|All files (*.*)|*.*'
    if ($null -ne $selected) { $mediaText.Text = $selected }
})

$refreshStatusButton.Add_Click({ Refresh-SystemStatus })
$refreshProfilesButton.Add_Click({ Refresh-Profiles })

$telemetryButton.Add_Click({
    try {
        Set-Activity (Start-MpcvrTool `
            -ScriptPath $telemetryScript `
            -Arguments @('-WaitSeconds', '10') `
            -Description 'Live telemetry reader')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$rollbackButton.Add_Click({
    try {
        Set-Activity (Start-MpcvrTool `
            -ScriptPath $rollbackScript `
            -Arguments @('-NoPause') `
            -Description 'Rollback')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$firstRunButton.Add_Click({
    try {
        if ([string]::IsNullOrWhiteSpace($mediaText.Text)) { throw 'Choose a calibration video first.' }
        Set-Activity (Start-MpcvrTool `
            -ScriptPath $firstRunScript `
            -Arguments (Get-CommonArguments -Mode 'Automatic' -Priority 'Balanced') `
            -Description 'Automatic first-run setup')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$installOnlyButton.Add_Click({
    try {
        $arguments = @('-Mode', 'Automatic', '-NoPause')
        if (-not [string]::IsNullOrWhiteSpace($sdkText.Text)) {
            $arguments += @('-NvOffrucSdkPath', (Quote-Argument -Value $sdkText.Text))
        }
        Set-Activity (Start-MpcvrTool -ScriptPath $installerScript -Arguments $arguments -Description 'Unified install/update')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$autoTuneOnlyButton.Add_Click({
    try {
        if ([string]::IsNullOrWhiteSpace($mediaText.Text)) { throw 'Choose a calibration video first.' }
        $arguments = @('-MediaPath', (Quote-Argument -Value $mediaText.Text), '-Priority', 'Balanced')
        if (-not [string]::IsNullOrWhiteSpace($automaticProfileName.Text)) {
            $arguments += @('-ProfileName', (Quote-Argument -Value $automaticProfileName.Text))
        }
        if ($automaticLock.Checked) { $arguments += '-LockProfile' }
        Set-Activity (Start-MpcvrTool -ScriptPath $autoTuneScript -Arguments $arguments -Description 'Automatic calibration')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$guidedRunButton.Add_Click({
    try {
        if ([string]::IsNullOrWhiteSpace($mediaText.Text)) { throw 'Choose a calibration video first.' }
        Set-Activity (Start-MpcvrTool `
            -ScriptPath $firstRunScript `
            -Arguments (Get-CommonArguments -Mode 'Guided' -Priority ([string]$priorityCombo.SelectedItem)) `
            -Description 'Guided first-run setup')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$guidedTuneButton.Add_Click({
    try {
        if ([string]::IsNullOrWhiteSpace($mediaText.Text)) { throw 'Choose a calibration video first.' }
        $arguments = @(
            '-MediaPath', (Quote-Argument -Value $mediaText.Text),
            '-Priority', ([string]$priorityCombo.SelectedItem)
        )
        Set-Activity (Start-MpcvrTool -ScriptPath $autoTuneScript -Arguments $arguments -Description 'Guided automatic calibration')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$previewProfileButton.Add_Click({
    try {
        if ($null -eq $profileCombo.SelectedItem) { throw 'Select a stored profile first.' }
        Set-Activity (Start-MpcvrTool `
            -ScriptPath $applyScript `
            -Arguments @('-Action', 'Preview', '-ProfileName', (Quote-Argument -Value ([string]$profileCombo.SelectedItem))) `
            -Description 'Profile preview')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$applyProfileButton.Add_Click({
    try {
        if ($null -eq $profileCombo.SelectedItem) { throw 'Select a stored profile first.' }
        Set-Activity (Start-MpcvrTool `
            -ScriptPath $applyScript `
            -Arguments @('-Action', 'Apply', '-ProfileName', (Quote-Argument -Value ([string]$profileCombo.SelectedItem))) `
            -Description 'Profile application')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$currentSettingsButton.Add_Click({
    try {
        Set-Activity (Start-MpcvrTool -ScriptPath $applyScript -Arguments @('-Action', 'Show') -Description 'Current renderer settings')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$manageProfilesButton.Add_Click({
    try {
        Set-Activity (Start-MpcvrTool -ScriptPath $profileManagerScript -Arguments @('-Action', 'List') -Description 'Profile manager')
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$openProfilesButton.Add_Click({
    try {
        $root = Get-MpcvrProfileRoot
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        [void](Start-Process -FilePath 'explorer.exe' -ArgumentList (Quote-Argument -Value $root))
        Set-Activity 'Profile folder opened.'
    }
    catch { Set-Activity $_.Exception.Message ([System.Drawing.Color]::DarkRed) }
})

$closeButton.Add_Click({ $form.Close() })
$form.Add_Shown({
    Refresh-SystemStatus
    Refresh-Profiles
})

[void]$form.ShowDialog()
$form.Dispose()





