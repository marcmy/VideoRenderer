#requires -Version 5.1

# Pinned or copied shortcuts from when the updater only supported K-Lite still run this name.
[CmdletBinding()]
param(
    [string]$PackageArchive,
    [string]$ChecksumFile,
    [switch]$NoPause,
    [switch]$ValidateOnly
)

& (Join-Path $PSScriptRoot 'Update-MPCVR.ps1') @PSBoundParameters
exit $LASTEXITCODE
