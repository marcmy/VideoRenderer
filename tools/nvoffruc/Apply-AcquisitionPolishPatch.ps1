#requires -Version 5.1

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')

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

$modulePath = Join-Path $repoRoot 'tools\nvoffruc\MpcvrNvOffruc.Acquisition.psm1'
$guiPath = Join-Path $repoRoot 'tools\UnifiedSetup\Start-MpcvrUnifiedSetup.ps1'
$readmePath = Join-Path $repoRoot 'tools\UnifiedSetup\README.md'

Replace-RequiredText -Path $modulePath -Old @'
            $entryNames = @($archive.Entries | ForEach-Object {
                $_.FullName.Replace('\\', '/').TrimStart('/')
            })
'@ -New @'
            $entryNames = @($archive.Entries | ForEach-Object {
                (([string]$_.FullName) -replace '\\', '/').TrimStart('/')
            })
'@

Replace-RequiredText -Path $modulePath -Old @'
                if (@($entryNames | Where-Object { $_.EndsWith($suffix, [StringComparison]::OrdinalIgnoreCase) }).Count -eq 0) {
'@ -New @'
                if (@($entryNames | Where-Object { $_ -like "*$suffix" }).Count -eq 0) {
'@

Replace-RequiredText -Path $guiPath -Old @'
$inputsGroup.Controls.Add((New-Label -Text 'Optical Flow SDK ZIP' -X 14 -Y 27 -Width 145))
$sdkText = New-Object System.Windows.Forms.TextBox
$sdkText.Location = New-Object System.Drawing.Point(164, 24)
$sdkText.Size = New-Object System.Drawing.Size(675, 24)
$inputsGroup.Controls.Add($sdkText)
$sdkBrowse = New-Button -Text 'Browse…' -X 850 -Y 21 -Width 78 -Height 28
$inputsGroup.Controls.Add($sdkBrowse)
'@ -New @'
$inputsGroup.Controls.Add((New-Label -Text 'Optical Flow SDK (optional)' -X 14 -Y 27 -Width 175))
$sdkText = New-Object System.Windows.Forms.TextBox
$sdkText.Location = New-Object System.Drawing.Point(194, 24)
$sdkText.Size = New-Object System.Drawing.Size(645, 24)
$inputsGroup.Controls.Add($sdkText)
$sdkBrowse = New-Button -Text 'Browse…' -X 850 -Y 21 -Width 78 -Height 28
$inputsGroup.Controls.Add($sdkBrowse)
$sdkToolTip = New-Object System.Windows.Forms.ToolTip
$sdkToolTip.SetToolTip($sdkText, 'Leave blank. Setup will open NVIDIA''s official login/download page, detect the completed SDK ZIP, validate it, and continue automatically.')
$sdkToolTip.SetToolTip($sdkBrowse, 'Optional manual fallback for an SDK ZIP you already downloaded.')
'@

Replace-RequiredText -Path $guiPath -Old @'
$automaticDescription = New-Label -Text 'Install or update all components, then test ranked settings until one passes. Original settings are restored when no candidate passes.' -X 18 -Y 20 -Width 870 -Height 42
'@ -New @'
$automaticDescription = New-Label -Text 'Install or update all components, acquiring the Optical Flow SDK through NVIDIA''s official page when needed, then test ranked settings until one passes. Original settings are restored when no candidate passes.' -X 18 -Y 20 -Width 870 -Height 52
'@

Replace-RequiredText -Path $readmePath -Old @'
Close MPC-HC, then run:

```powershell
.\tools\UnifiedSetup\Install-MpcvrUnified.cmd `
  -NvOffrucSdkPath "C:\Path\To\Optical_Flow_SDK_5.0.7.zip"
```

When renderer and Maxine payloads are not embedded, their existing installers can obtain the latest published files. NvOFFRUC still requires the official NVIDIA Optical Flow SDK archive unless a complete runtime is already installed.
'@ -New @'
Close MPC-HC, then run:

```powershell
.\tools\UnifiedSetup\Install-MpcvrUnified.cmd
```

Renderer and the verified slim Maxine runtime are embedded in the unified release package. When NvOFFRUC is not already installed and no SDK path is supplied, setup opens NVIDIA's official secured Optical Flow SDK download page. The user signs in and accepts NVIDIA's license in the browser; setup watches the Windows Downloads folder, recognizes the completed compatible ZIP, validates and caches it, extracts only the required runtime files, and continues automatically.

Manual `-NvOffrucSdkPath` selection remains available as an offline or Advanced fallback. `-DisableOfficialDownload` skips the browser-assisted path. `-OfficialDownloadWaitMinutes` changes the default 15-minute detection window.
'@

Write-Host 'Acquisition compatibility and GUI polish applied.' -ForegroundColor Green
