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
$installerScript = Join-Path $PSScriptRoot 'Install-MpcvrUnified.ps1'
$rollbackScript = Join-Path $PSScriptRoot 'Restore-MpcvrUnifiedBackup.ps1'
$telemetryScript = Join-Path $PSScriptRoot 'Get-MpcvrRendererTelemetry.ps1'

foreach ($path in @($commonModule, $installerScript, $rollbackScript, $telemetryScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Unified setup dependency is missing: $path"
    }
}

Import-Module -Name $commonModule -Force

function Get-PowerShellExecutable {
    $pwsh = Get-Command 'pwsh.exe' -ErrorAction SilentlyContinue
    if ($null -ne $pwsh) {
        return $pwsh.Source
    }

    $powershell = Get-Command 'powershell.exe' -ErrorAction SilentlyContinue
    if ($null -ne $powershell) {
        return $powershell.Source
    }

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
        '-NoLogo',
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        (Quote-Argument -Value $ScriptPath)
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
    param(
        [string]$Text,
        [int]$X,
        [int]$Y,
        [int]$Width = 150,
        [int]$Height = 22
    )

    $control = New-Object System.Windows.Forms.Label
    $control.Text = $Text
    $control.Location = New-Object System.Drawing.Point($X, $Y)
    $control.Size = New-Object System.Drawing.Size($Width, $Height)
    return $control
}

function New-Button {
    param(
        [string]$Text,
        [int]$X,
        [int]$Y,
        [int]$Width = 145,
        [int]$Height = 32
    )

    $control = New-Object System.Windows.Forms.Button
    $control.Text = $Text
    $control.Location = New-Object System.Drawing.Point($X, $Y)
    $control.Size = New-Object System.Drawing.Size($Width, $Height)
    return $control
}

function Get-MpcvrOptionalProperty {
    param(
        [object]$InputObject,
        [Parameter(Mandatory)]
        [string]$Name,
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

function ConvertTo-MpcvrSimpleStatusLines {
    param(
        [Parameter(Mandatory)]
        [object]$Profile
    )

    $lines = @()
    $gpus = @(Get-MpcvrOptionalProperty -InputObject $Profile -Name 'Gpus' -DefaultValue @())
    if ($gpus.Count -gt 0) {
        $gpu = $gpus[0]
        $name = [string](Get-MpcvrOptionalProperty -InputObject $gpu -Name 'Name' -DefaultValue 'Unknown NVIDIA GPU')
        $driver = [string](Get-MpcvrOptionalProperty -InputObject $gpu -Name 'DriverVersion' -DefaultValue 'unknown')
        $memoryMiB = [double](Get-MpcvrOptionalProperty -InputObject $gpu -Name 'MemoryMiB' -DefaultValue 0)
        $vram = if ($memoryMiB -gt 0) {
            '{0:N1} GB' -f ($memoryMiB / 1024.0)
        }
        else {
            'VRAM unknown'
        }
        $lines += "NVIDIA GPU: $name ($vram, driver $driver)"
    }
    else {
        $lines += 'NVIDIA GPU: Not detected'
    }

    $players = @(Get-MpcvrOptionalProperty -InputObject $Profile -Name 'Players' -DefaultValue @())
    $availablePlayers = @($players | Where-Object {
        [bool](Get-MpcvrOptionalProperty -InputObject $_ -Name 'DirectoryExists' -DefaultValue $false)
    })
    if ($availablePlayers.Count -gt 0) {
        $playerNames = @($availablePlayers | ForEach-Object {
            [string](Get-MpcvrOptionalProperty -InputObject $_ -Name 'Name' -DefaultValue 'MPC-HC')
        })
        $lines += 'MPC-HC / K-Lite: Found (' + ($playerNames -join ', ') + ')'
    }
    else {
        $lines += 'MPC-HC / K-Lite: Not found'
    }

    $rendererInstalled = @($players | Where-Object {
        [bool](Get-MpcvrOptionalProperty -InputObject $_ -Name 'RendererExists' -DefaultValue $false)
    }).Count -gt 0
    $lines += 'MPC Video Renderer: ' + $(if ($rendererInstalled) { 'Installed - setup will update it' } else { 'Ready to install' })

    $runtimes = Get-MpcvrOptionalProperty -InputObject $Profile -Name 'Runtimes'
    $maxine = Get-MpcvrOptionalProperty -InputObject $runtimes -Name 'Maxine'
    $fruc = Get-MpcvrOptionalProperty -InputObject $runtimes -Name 'NvOFFRUC'

    $maxineInstalled = [bool](Get-MpcvrOptionalProperty -InputObject $maxine -Name 'Installed' -DefaultValue $false)
    $frucInstalled = [bool](Get-MpcvrOptionalProperty -InputObject $fruc -Name 'Installed' -DefaultValue $false)

    $lines += 'NVIDIA Maxine: ' + $(if ($maxineInstalled) { 'Installed - setup will verify/update it' } else { 'Ready to install' })
    $lines += 'Frame interpolation: ' + $(if ($frucInstalled) { 'Installed - setup will preserve/verify it' } else { 'NVIDIA download required during setup' })

    return $lines
}

if ($ValidateOnly) {
    [void](Get-PowerShellExecutable)

    $synthetic = [pscustomobject]@{
        Gpus = @([pscustomobject]@{
            Name = 'Synthetic GPU'
            DriverVersion = '1.2.3'
            MemoryMiB = 8192
        })
        Players = @([pscustomobject]@{
            Name = 'Synthetic MPC-HC x64'
            DirectoryExists = $true
            RendererExists = $true
        })
        Runtimes = [pscustomobject]@{
            Maxine = [pscustomobject]@{ Installed = $true }
            NvOFFRUC = [pscustomobject]@{ Installed = $false }
        }
    }

    $statusLines = @(ConvertTo-MpcvrSimpleStatusLines -Profile $synthetic)
    if ($statusLines.Count -ne 5 -or
        $statusLines[0] -notmatch '8\.0 GB' -or
        $statusLines[4] -notmatch 'download required') {
        throw 'Simplified GUI status validation failed.'
    }

    $testForm = New-Object System.Windows.Forms.Form
    $testForm.Text = 'MPCVR Unified Setup Validation'
    $testForm.Controls.Add((New-Button -Text 'Install / Update' -X 10 -Y 10))
    $testForm.Dispose()

    Write-Host 'Simplified MPCVR Unified Setup GUI validation passed.' -ForegroundColor Green
    exit 0
}

$advancedState = [pscustomobject]@{
    SdkPath = ''
    WaitMinutes = 15
    SkipRenderer = $false
    SkipMaxine = $false
    SkipNvOffruc = $false
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'MPCVR Enhanced Setup'
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object System.Drawing.Size(680, 500)
$form.MinimumSize = New-Object System.Drawing.Size(696, 539)
$form.MaximizeBox = $false
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)

$title = New-Label `
    -Text 'MPC Video Renderer Enhanced Setup' `
    -X 22 -Y 18 -Width 600 -Height 34
$title.Font = New-Object System.Drawing.Font('Segoe UI Semibold', 17)
$form.Controls.Add($title)

$subtitle = New-Label `
    -Text 'Installs MPC Video Renderer, NVIDIA Maxine enhancement, and frame interpolation.' `
    -X 24 -Y 55 -Width 620 -Height 28
$subtitle.ForeColor = [System.Drawing.Color]::DimGray
$form.Controls.Add($subtitle)

$statusGroup = New-Object System.Windows.Forms.GroupBox
$statusGroup.Text = 'System check'
$statusGroup.Location = New-Object System.Drawing.Point(22, 94)
$statusGroup.Size = New-Object System.Drawing.Size(636, 190)
$form.Controls.Add($statusGroup)

$statusText = New-Object System.Windows.Forms.TextBox
$statusText.Location = New-Object System.Drawing.Point(16, 27)
$statusText.Size = New-Object System.Drawing.Size(604, 145)
$statusText.Multiline = $true
$statusText.ReadOnly = $true
$statusText.ScrollBars = 'Vertical'
$statusText.BackColor = [System.Drawing.SystemColors]::Window
$statusText.Font = New-Object System.Drawing.Font('Segoe UI', 9.5)
$statusGroup.Controls.Add($statusText)

$installButton = New-Button -Text 'Install / Update' -X 190 -Y 306 -Width 300 -Height 62
$installButton.Font = New-Object System.Drawing.Font('Segoe UI Semibold', 13)
$form.Controls.Add($installButton)

$licenseNote = New-Label `
    -Text 'If frame interpolation is not already installed, NVIDIA may ask you to sign in and accept its SDK license. Setup handles the rest.' `
    -X 60 -Y 380 -Width 560 -Height 48
$licenseNote.TextAlign = 'MiddleCenter'
$licenseNote.ForeColor = [System.Drawing.Color]::DimGray
$form.Controls.Add($licenseNote)

$refreshButton = New-Button -Text 'Refresh' -X 22 -Y 447 -Width 95 -Height 32
$form.Controls.Add($refreshButton)
$advancedButton = New-Button -Text 'Advanced...' -X 125 -Y 447 -Width 110 -Height 32
$form.Controls.Add($advancedButton)
$closeButton = New-Button -Text 'Close' -X 563 -Y 447 -Width 95 -Height 32
$form.Controls.Add($closeButton)

$activityLabel = New-Label -Text 'Ready.' -X 250 -Y 452 -Width 295 -Height 24
$activityLabel.TextAlign = 'MiddleCenter'
$activityLabel.ForeColor = [System.Drawing.Color]::DarkSlateGray
$form.Controls.Add($activityLabel)

function Set-Activity {
    param(
        [string]$Text,
        [System.Drawing.Color]$Color = [System.Drawing.Color]::DarkSlateGray
    )

    $activityLabel.Text = $Text
    $activityLabel.ForeColor = $Color
}

function Refresh-SystemStatus {
    try {
        $profile = Get-MpcvrSystemProfile
        $statusText.Lines = @(ConvertTo-MpcvrSimpleStatusLines -Profile $profile)
        Set-Activity -Text 'System check complete.' -Color ([System.Drawing.Color]::DarkGreen)
    }
    catch {
        $statusText.Text = $_.Exception.Message
        Set-Activity -Text 'System check failed.' -Color ([System.Drawing.Color]::DarkRed)
    }
}

function Show-AdvancedOptions {
    $advancedForm = New-Object System.Windows.Forms.Form
    $advancedForm.Text = 'Advanced setup options'
    $advancedForm.StartPosition = 'CenterParent'
    $advancedForm.ClientSize = New-Object System.Drawing.Size(610, 360)
    $advancedForm.FormBorderStyle = 'FixedDialog'
    $advancedForm.MaximizeBox = $false
    $advancedForm.MinimizeBox = $false
    $advancedForm.ShowInTaskbar = $false
    $advancedForm.Font = New-Object System.Drawing.Font('Segoe UI', 9)

    $description = New-Label `
        -Text 'These options are not needed for a normal installation.' `
        -X 18 -Y 15 -Width 560 -Height 28
    $description.ForeColor = [System.Drawing.Color]::DimGray
    $advancedForm.Controls.Add($description)

    $advancedForm.Controls.Add((New-Label -Text 'Existing Optical Flow SDK ZIP or folder' -X 18 -Y 58 -Width 235))
    $sdkText = New-Object System.Windows.Forms.TextBox
    $sdkText.Location = New-Object System.Drawing.Point(18, 82)
    $sdkText.Size = New-Object System.Drawing.Size(468, 24)
    $sdkText.Text = [string]$advancedState.SdkPath
    $advancedForm.Controls.Add($sdkText)

    $browseButton = New-Button -Text 'Browse...' -X 496 -Y 78 -Width 95 -Height 30
    $advancedForm.Controls.Add($browseButton)

    $advancedForm.Controls.Add((New-Label -Text 'Official download wait time' -X 18 -Y 126 -Width 180))
    $waitNumeric = New-Object System.Windows.Forms.NumericUpDown
    $waitNumeric.Location = New-Object System.Drawing.Point(205, 123)
    $waitNumeric.Size = New-Object System.Drawing.Size(72, 24)
    $waitNumeric.Minimum = 1
    $waitNumeric.Maximum = 60
    $waitNumeric.Value = [decimal]$advancedState.WaitMinutes
    $advancedForm.Controls.Add($waitNumeric)
    $advancedForm.Controls.Add((New-Label -Text 'minutes' -X 285 -Y 126 -Width 70))

    $skipRenderer = New-Object System.Windows.Forms.CheckBox
    $skipRenderer.Text = 'Do not install or update MPC Video Renderer'
    $skipRenderer.Location = New-Object System.Drawing.Point(18, 170)
    $skipRenderer.Size = New-Object System.Drawing.Size(330, 24)
    $skipRenderer.Checked = [bool]$advancedState.SkipRenderer
    $advancedForm.Controls.Add($skipRenderer)

    $skipMaxine = New-Object System.Windows.Forms.CheckBox
    $skipMaxine.Text = 'Do not install or update NVIDIA Maxine'
    $skipMaxine.Location = New-Object System.Drawing.Point(18, 200)
    $skipMaxine.Size = New-Object System.Drawing.Size(330, 24)
    $skipMaxine.Checked = [bool]$advancedState.SkipMaxine
    $advancedForm.Controls.Add($skipMaxine)

    $skipNvOffruc = New-Object System.Windows.Forms.CheckBox
    $skipNvOffruc.Text = 'Do not install or update frame interpolation'
    $skipNvOffruc.Location = New-Object System.Drawing.Point(18, 230)
    $skipNvOffruc.Size = New-Object System.Drawing.Size(330, 24)
    $skipNvOffruc.Checked = [bool]$advancedState.SkipNvOffruc
    $advancedForm.Controls.Add($skipNvOffruc)

    $telemetryButton = New-Button -Text 'Live telemetry' -X 365 -Y 166 -Width 145 -Height 34
    $advancedForm.Controls.Add($telemetryButton)
    $rollbackButton = New-Button -Text 'Rollback last install' -X 365 -Y 210 -Width 145 -Height 34
    $advancedForm.Controls.Add($rollbackButton)
    $openFolderButton = New-Button -Text 'Open setup folder' -X 365 -Y 254 -Width 145 -Height 34
    $advancedForm.Controls.Add($openFolderButton)

    $saveButton = New-Button -Text 'Save' -X 390 -Y 314 -Width 95 -Height 32
    $saveButton.DialogResult = [System.Windows.Forms.DialogResult]::OK
    $advancedForm.AcceptButton = $saveButton
    $advancedForm.Controls.Add($saveButton)

    $cancelButton = New-Button -Text 'Cancel' -X 496 -Y 314 -Width 95 -Height 32
    $cancelButton.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $advancedForm.CancelButton = $cancelButton
    $advancedForm.Controls.Add($cancelButton)

    $browseButton.Add_Click({
        $selected = Select-MpcvrFile `
            -Title 'Select NVIDIA Optical Flow SDK ZIP' `
            -Filter 'NVIDIA Optical Flow SDK ZIP (*.zip)|*.zip|All files (*.*)|*.*'
        if ($null -ne $selected) {
            $sdkText.Text = $selected
        }
    })

    $telemetryButton.Add_Click({
        try {
            [void](Start-MpcvrTool `
                -ScriptPath $telemetryScript `
                -Arguments @('-WaitSeconds', '10') `
                -Description 'Live telemetry')
        }
        catch {
            [System.Windows.Forms.MessageBox]::Show(
                $_.Exception.Message,
                'MPCVR Enhanced Setup',
                [System.Windows.Forms.MessageBoxButtons]::OK,
                [System.Windows.Forms.MessageBoxIcon]::Error
            ) | Out-Null
        }
    })

    $rollbackButton.Add_Click({
        try {
            [void](Start-MpcvrTool `
                -ScriptPath $rollbackScript `
                -Description 'Rollback')
        }
        catch {
            [System.Windows.Forms.MessageBox]::Show(
                $_.Exception.Message,
                'MPCVR Enhanced Setup',
                [System.Windows.Forms.MessageBoxButtons]::OK,
                [System.Windows.Forms.MessageBoxIcon]::Error
            ) | Out-Null
        }
    })

    $openFolderButton.Add_Click({
        [void](Start-Process -FilePath 'explorer.exe' -ArgumentList (Quote-Argument -Value $PSScriptRoot))
    })

    $result = $advancedForm.ShowDialog($form)
    if ($result -eq [System.Windows.Forms.DialogResult]::OK) {
        $advancedState.SdkPath = $sdkText.Text.Trim()
        $advancedState.WaitMinutes = [int]$waitNumeric.Value
        $advancedState.SkipRenderer = $skipRenderer.Checked
        $advancedState.SkipMaxine = $skipMaxine.Checked
        $advancedState.SkipNvOffruc = $skipNvOffruc.Checked
        Set-Activity -Text 'Advanced options saved.'
    }

    $advancedForm.Dispose()
}

$installButton.Add_Click({
    try {
        $arguments = @(
            '-Mode',
            'Automatic',
            '-OfficialDownloadWaitMinutes',
            [string]$advancedState.WaitMinutes
        )

        if (-not [string]::IsNullOrWhiteSpace([string]$advancedState.SdkPath)) {
            $arguments += @(
                '-NvOffrucSdkPath',
                (Quote-Argument -Value ([string]$advancedState.SdkPath))
            )
        }
        if ([bool]$advancedState.SkipRenderer) {
            $arguments += '-SkipRenderer'
        }
        if ([bool]$advancedState.SkipMaxine) {
            $arguments += '-SkipMaxine'
        }
        if ([bool]$advancedState.SkipNvOffruc) {
            $arguments += '-SkipNvOffruc'
        }

        Set-Activity -Text (Start-MpcvrTool `
            -ScriptPath $installerScript `
            -Arguments $arguments `
            -Description 'Installation') `
            -Color ([System.Drawing.Color]::DarkGreen)
    }
    catch {
        Set-Activity -Text $_.Exception.Message -Color ([System.Drawing.Color]::DarkRed)
    }
})

$refreshButton.Add_Click({ Refresh-SystemStatus })
$advancedButton.Add_Click({ Show-AdvancedOptions })
$closeButton.Add_Click({ $form.Close() })
$form.Add_Shown({ Refresh-SystemStatus })

[void]$form.ShowDialog()
$form.Dispose()
