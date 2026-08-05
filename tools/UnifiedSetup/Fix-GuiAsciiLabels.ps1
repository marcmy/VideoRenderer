#requires -Version 5.1

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$guiPath = Join-Path $PSScriptRoot 'Start-MpcvrUnifiedSetup.ps1'
$lines = @(Get-Content -LiteralPath $guiPath)
$foundSdk = $false
$foundMedia = $false

for ($index = 0; $index -lt $lines.Count; $index++) {
    if ($lines[$index] -match '^\$sdkBrowse\s*=') {
        $lines[$index] = '$sdkBrowse = New-Button -Text ''Browse...'' -X 850 -Y 21 -Width 78 -Height 28'
        $foundSdk = $true
    }
    elseif ($lines[$index] -match '^\$mediaBrowse\s*=') {
        $lines[$index] = '$mediaBrowse = New-Button -Text ''Browse...'' -X 850 -Y 61 -Width 78 -Height 28'
        $foundMedia = $true
    }
}

if (-not $foundSdk -or -not $foundMedia) {
    throw "Expected Browse button lines were not both found. SDK=$foundSdk Media=$foundMedia"
}

Set-Content -LiteralPath $guiPath -Value $lines -Encoding UTF8
Write-Host 'GUI ASCII label cleanup applied.' -ForegroundColor Green
