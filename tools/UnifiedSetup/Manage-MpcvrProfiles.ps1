#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('List', 'Create', 'Show', 'Validate', 'Import', 'Export', 'Duplicate', 'Lock', 'Unlock', 'Delete', 'RestoreDefaults')]
    [string]$Action = 'List',
    [string]$Name,
    [string]$NewName,
    [ValidateSet('Automatic', 'Guided', 'Advanced')]
    [string]$Mode = 'Automatic',
    [ValidateSet('up-to-30', 'over-30-to-60', 'custom')]
    [string]$SourceFrameRateClass = 'up-to-30',
    [string]$Source,
    [string]$Destination,
    [string]$ProfileRoot,
    [switch]$Locked,
    [switch]$Force,
    [switch]$AllowLockedOverwrite,
    [switch]$AllowLockedDelete,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$modulePath = Join-Path $PSScriptRoot 'MpcvrSetup.Profiles.psm1'
if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    throw "Profile module is missing: $modulePath"
}
Import-Module -Name $modulePath -Force

function Assert-Value {
    param(
        [string]$Value,
        [string]$ParameterName
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "$ParameterName is required for action $Action."
    }
}

function Write-ProfileResult {
    param(
        [Parameter(Mandatory)]
        [object]$Value
    )

    if ($Json) {
        $Value | ConvertTo-Json -Depth 12
    }
    else {
        $Value
    }
}

switch ($Action) {
    'List' {
        $profiles = @(Get-MpcvrProfiles -ProfileRoot $ProfileRoot)
        if ($Json) {
            $profiles | Select-Object Name, Mode, Locked, SourceFrameRateClass, Path, Valid, Errors |
                ConvertTo-Json -Depth 6
        }
        elseif ($profiles.Count -eq 0) {
            Write-Host 'No MPCVR profiles are stored.' -ForegroundColor Yellow
        }
        else {
            $profiles |
                Select-Object Name, Mode, Locked, SourceFrameRateClass, Valid, Path |
                Format-Table -AutoSize
        }
    }

    'Create' {
        Assert-Value -Value $Name -ParameterName 'Name'
        $profile = New-MpcvrProfile `
            -Name $Name `
            -Mode $Mode `
            -SourceFrameRateClass $SourceFrameRateClass `
            -Locked:$Locked
        $path = Save-MpcvrProfile `
            -Profile $profile `
            -ProfileRoot $ProfileRoot `
            -Force:$Force `
            -AllowLockedOverwrite:$AllowLockedOverwrite
        Write-ProfileResult -Value ([pscustomobject]@{ Action = 'Created'; Name = $profile.name; Path = $path; Locked = $profile.locked })
    }

    'Show' {
        Assert-Value -Value $Name -ParameterName 'Name'
        $found = Find-MpcvrProfile -Name $Name -ProfileRoot $ProfileRoot
        if ($Json) {
            $found.Profile | ConvertTo-Json -Depth 12
        }
        else {
            Write-Host "Profile: $($found.Profile.name)" -ForegroundColor Cyan
            Write-Host "Path: $($found.Path)"
            $found.Profile | Format-List
        }
    }

    'Validate' {
        if (-not [string]::IsNullOrWhiteSpace($Source)) {
            $profile = Read-MpcvrProfile -Path $Source -AllowInvalid
            $location = (Resolve-Path -LiteralPath $Source).Path
        }
        else {
            Assert-Value -Value $Name -ParameterName 'Name or Source'
            $found = Find-MpcvrProfile -Name $Name -ProfileRoot $ProfileRoot
            $profile = $found.Profile
            $location = $found.Path
        }
        $validation = Test-MpcvrProfile -Profile $profile
        $result = [pscustomobject]@{
            Valid = $validation.Valid
            Location = $location
            Errors = $validation.Errors
        }
        Write-ProfileResult -Value $result
        if (-not $validation.Valid) {
            exit 1
        }
    }

    'Import' {
        Assert-Value -Value $Source -ParameterName 'Source'
        $path = Import-MpcvrProfile `
            -Source $Source `
            -Name $Name `
            -ProfileRoot $ProfileRoot `
            -Force:$Force `
            -AllowLockedOverwrite:$AllowLockedOverwrite
        $imported = Read-MpcvrProfile -Path $path
        Write-ProfileResult -Value ([pscustomobject]@{ Action = 'Imported'; Name = $imported.name; Path = $path; Locked = $imported.locked })
    }

    'Export' {
        Assert-Value -Value $Name -ParameterName 'Name'
        Assert-Value -Value $Destination -ParameterName 'Destination'
        $path = Export-MpcvrProfile `
            -Name $Name `
            -Destination $Destination `
            -ProfileRoot $ProfileRoot `
            -Force:$Force
        Write-ProfileResult -Value ([pscustomobject]@{ Action = 'Exported'; Name = $Name; Path = $path })
    }

    'Duplicate' {
        Assert-Value -Value $Name -ParameterName 'Name'
        Assert-Value -Value $NewName -ParameterName 'NewName'
        $found = Find-MpcvrProfile -Name $Name -ProfileRoot $ProfileRoot
        $copy = $found.Profile | ConvertTo-Json -Depth 12 | ConvertFrom-Json
        $copy.name = $NewName.Trim()
        $copy.locked = [bool]$Locked
        $path = Save-MpcvrProfile `
            -Profile $copy `
            -ProfileRoot $ProfileRoot `
            -Force:$Force `
            -AllowLockedOverwrite:$AllowLockedOverwrite
        Write-ProfileResult -Value ([pscustomobject]@{ Action = 'Duplicated'; Source = $Name; Name = $copy.name; Path = $path; Locked = $copy.locked })
    }

    'Lock' {
        Assert-Value -Value $Name -ParameterName 'Name'
        $path = Set-MpcvrProfileLock -Name $Name -Locked $true -ProfileRoot $ProfileRoot
        Write-ProfileResult -Value ([pscustomobject]@{ Action = 'Locked'; Name = $Name; Path = $path })
    }

    'Unlock' {
        Assert-Value -Value $Name -ParameterName 'Name'
        $path = Set-MpcvrProfileLock -Name $Name -Locked $false -ProfileRoot $ProfileRoot
        Write-ProfileResult -Value ([pscustomobject]@{ Action = 'Unlocked'; Name = $Name; Path = $path })
    }

    'Delete' {
        Assert-Value -Value $Name -ParameterName 'Name'
        Remove-MpcvrProfile `
            -Name $Name `
            -ProfileRoot $ProfileRoot `
            -AllowLockedDelete:$AllowLockedDelete
        Write-ProfileResult -Value ([pscustomobject]@{ Action = 'Deleted'; Name = $Name })
    }

    'RestoreDefaults' {
        $paths = @(Restore-MpcvrFactoryProfiles -ProfileRoot $ProfileRoot -Force:$Force)
        Write-ProfileResult -Value ([pscustomobject]@{ Action = 'RestoredDefaults'; Paths = $paths })
    }
}
