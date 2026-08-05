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
    $updated = $content.Replace($Old, $New)
    Set-Content -LiteralPath $Path -Value $updated -Encoding UTF8
}

$frucInstaller = Join-Path $repoRoot 'tools\nvoffruc\Install-NvOFFRUCRuntime.ps1'
$unifiedInstaller = Join-Path $repoRoot 'tools\UnifiedSetup\Install-MpcvrUnified.ps1'

Replace-RequiredText -Path $frucInstaller -Old @'
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR NvOFFRUC Runtime'),
    [switch]$ValidateOnly,
    [switch]$SkipEnvironmentUpdate,
    [switch]$NoPause
'@ -New @'
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR NvOFFRUC Runtime'),
    [ValidateRange(1, 60)]
    [int]$OfficialDownloadWaitMinutes = 15,
    [switch]$DisableOfficialDownload,
    [switch]$AllowUnverifiedRuntimeFiles,
    [switch]$ValidateOnly,
    [switch]$SkipEnvironmentUpdate,
    [switch]$NoPause
'@

Replace-RequiredText -Path $frucInstaller -Old @'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 2.0

function Complete-Run {
'@ -New @'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 2.0

$acquisitionModule = Join-Path $PSScriptRoot 'MpcvrNvOffruc.Acquisition.psm1'
if (-not (Test-Path -LiteralPath $acquisitionModule -PathType Leaf)) {
    throw "The NvOFFRUC acquisition module is missing: $acquisitionModule"
}
Import-Module -Name $acquisitionModule -Force

function Complete-Run {
'@

Replace-RequiredText -Path $frucInstaller -Old @'
    if ([string]::IsNullOrWhiteSpace($SdkPath)) {
        $SdkPath = Select-SdkPackage
    }
'@ -New @'
    if ([string]::IsNullOrWhiteSpace($SdkPath)) {
        $SdkPath = Resolve-MpcvrOpticalFlowSdkPackage `
            -PayloadDirectory $PSScriptRoot `
            -WaitMinutes $OfficialDownloadWaitMinutes `
            -DisableOfficialDownload:$DisableOfficialDownload
    }
'@

Replace-RequiredText -Path $frucInstaller -Old @'
    $source = Get-NvOffrucSource -SearchRoot $searchRoot
    $staging = "$InstallRoot.staging-$([guid]::NewGuid().ToString('N'))"
'@ -New @'
    $source = Get-NvOffrucSource -SearchRoot $searchRoot
    [void](Test-MpcvrNvOffrucRuntimeHashes `
        -NvOffrucPath $source.NvOffruc `
        -CudaRuntimePath $source.CudaRuntime `
        -AllowUnverifiedRuntimeFiles:$AllowUnverifiedRuntimeFiles)

    $staging = "$InstallRoot.staging-$([guid]::NewGuid().ToString('N'))"
'@

Replace-RequiredText -Path $unifiedInstaller -Old @'
    [string]$MaxineRuntimeArchive,
    [string]$NvOffrucSdkPath,
    [ValidateSet('Automatic', 'Guided', 'Advanced')]
'@ -New @'
    [string]$MaxineRuntimeArchive,
    [string]$NvOffrucSdkPath,
    [ValidateRange(1, 60)]
    [int]$OfficialDownloadWaitMinutes = 15,
    [switch]$DisableOfficialDownload,
    [switch]$AllowUnverifiedRuntimeFiles,
    [ValidateSet('Automatic', 'Guided', 'Advanced')]
'@

Replace-RequiredText -Path $unifiedInstaller -Old @'
    Add-OptionalPathArgument -List $parts -Name '-NvOffrucSdkPath' -Value $NvOffrucSdkPath
    Add-OptionalPathArgument -List $parts -Name '-DataRoot' -Value $DataRoot
    $parts.Add('-Mode')
'@ -New @'
    Add-OptionalPathArgument -List $parts -Name '-NvOffrucSdkPath' -Value $NvOffrucSdkPath
    Add-OptionalPathArgument -List $parts -Name '-DataRoot' -Value $DataRoot
    $parts.Add('-OfficialDownloadWaitMinutes')
    $parts.Add([string]$OfficialDownloadWaitMinutes)
    $parts.Add('-Mode')
'@

Replace-RequiredText -Path $unifiedInstaller -Old @'
    foreach ($switchName in @('SkipRenderer', 'SkipMaxine', 'SkipNvOffruc', 'NoPause')) {
'@ -New @'
    foreach ($switchName in @(
        'SkipRenderer',
        'SkipMaxine',
        'SkipNvOffruc',
        'DisableOfficialDownload',
        'AllowUnverifiedRuntimeFiles',
        'NoPause'
    )) {
'@

Replace-RequiredText -Path $unifiedInstaller -Old @'
    $profileBefore = Get-MpcvrSystemProfile
    if (-not $SkipNvOffruc -and
        [string]::IsNullOrWhiteSpace($NvOffrucSdkPath) -and
        -not [bool]$profileBefore.Runtimes.NvOFFRUC.Installed) {
        throw 'NvOFFRUC is not installed. Select Optical_Flow_SDK_5.0.7.zip or include it in the setup payload.'
    }

    $DataRoot = [IO.Path]::GetFullPath($DataRoot)
'@ -New @'
    $profileBefore = Get-MpcvrSystemProfile

    $DataRoot = [IO.Path]::GetFullPath($DataRoot)
'@

Replace-RequiredText -Path $unifiedInstaller -Old @'
    if (-not $SkipNvOffruc) {
        if (-not [string]::IsNullOrWhiteSpace($NvOffrucSdkPath)) {
            Invoke-MpcvrPowerShellScript `
                -Name '2/3 Installing NVIDIA NvOFFRUC runtime...' `
                -ScriptPath $frucInstaller `
                -Arguments @(
                    '-SdkPath',
                    (ConvertTo-MpcvrCommandLineArgument -Value $NvOffrucSdkPath),
                    '-NoPause'
                ) `
                -NoNewWindow
        }
        else {
            Write-Host
            Write-Host '2/3 Preserving the already installed NvOFFRUC runtime.' -ForegroundColor Cyan
        }
    }
'@ -New @'
    if (-not $SkipNvOffruc) {
        $shouldInstallNvOffruc =
            -not [string]::IsNullOrWhiteSpace($NvOffrucSdkPath) -or
            -not [bool]$profileBefore.Runtimes.NvOFFRUC.Installed

        if ($shouldInstallNvOffruc) {
            $frucArguments = New-Object 'System.Collections.Generic.List[string]'
            if (-not [string]::IsNullOrWhiteSpace($NvOffrucSdkPath)) {
                $frucArguments.Add('-SdkPath')
                $frucArguments.Add((ConvertTo-MpcvrCommandLineArgument -Value $NvOffrucSdkPath))
            }
            $frucArguments.Add('-OfficialDownloadWaitMinutes')
            $frucArguments.Add([string]$OfficialDownloadWaitMinutes)
            if ($DisableOfficialDownload) {
                $frucArguments.Add('-DisableOfficialDownload')
            }
            if ($AllowUnverifiedRuntimeFiles) {
                $frucArguments.Add('-AllowUnverifiedRuntimeFiles')
            }
            $frucArguments.Add('-NoPause')

            Invoke-MpcvrPowerShellScript `
                -Name '2/3 Installing NVIDIA NvOFFRUC runtime...' `
                -ScriptPath $frucInstaller `
                -Arguments @($frucArguments) `
                -NoNewWindow
        }
        else {
            Write-Host
            Write-Host '2/3 Preserving the already installed NvOFFRUC runtime.' -ForegroundColor Cyan
        }
    }
'@

Write-Host 'Assisted NVIDIA SDK acquisition integration applied.' -ForegroundColor Green
