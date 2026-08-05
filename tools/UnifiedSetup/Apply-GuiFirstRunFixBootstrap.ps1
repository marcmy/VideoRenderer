#requires -Version 5.1

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$patchPath = Join-Path $PSScriptRoot 'Apply-GuiFirstRunFix.ps1'
$guiPath = Join-Path $PSScriptRoot 'Start-MpcvrUnifiedSetup.ps1'

$patch = Get-Content -LiteralPath $patchPath -Raw
$patch = [regex]::Replace(
    $patch,
    '(?m)^Replace-RequiredText -Path \$guiPath -Old .*Browse.*\r?\n',
    '')
$patch = [regex]::Replace(
    $patch,
    '(?m)^Replace-RequiredText -Path \$guiPath -Old .*30.*60.*\r?\n',
    '')
$patch = [regex]::Replace(
    $patch,
    '(?m)^Replace-RequiredText -Path \$guiPath -Old .*60.*120.*\r?\n',
    '')
Set-Content -LiteralPath $patchPath -Value $patch -Encoding UTF8

& $patchPath

$gui = Get-Content -LiteralPath $guiPath -Raw
$gui = [regex]::Replace(
    $gui,
    '(?m)^\$sdkBrowse = New-Button -Text ''[^'']*'' -X 850 -Y 21 -Width 78 -Height 28$',
    '$sdkBrowse = New-Button -Text ''Browse...'' -X 850 -Y 21 -Width 78 -Height 28')
$gui = [regex]::Replace(
    $gui,
    '(?m)^\$mediaBrowse = New-Button -Text ''[^'']*'' -X 850 -Y 61 -Width 78 -Height 28$',
    '$mediaBrowse = New-Button -Text ''Browse...'' -X 850 -Y 61 -Width 78 -Height 28')
$gui = [regex]::Replace(
    $gui,
    '(?m)^\$automaticNote = New-Label -Text .*$',
    '$automaticNote = New-Label -Text ''Use a representative 30 fps video to build a 30 -> 60 profile and a representative 60 fps video to build a separate 60 -> 120 profile.'' -X 18 -Y 184 -Width 860 -Height 42')
Set-Content -LiteralPath $guiPath -Value $gui -Encoding UTF8

Write-Host 'Encoding-safe first-run GUI patch completed.' -ForegroundColor Green
