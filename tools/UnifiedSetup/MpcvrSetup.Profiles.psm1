#requires -Version 5.1

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Get-MpcvrProfileRoot {
    param(
        [string]$ProfileRoot = (Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup\profiles')
    )

    return [IO.Path]::GetFullPath($ProfileRoot)
}

function Get-MpcvrProfileNameHash {
    param(
        [Parameter(Mandatory)]
        [string]$Name
    )

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Name)
        $hash = $sha.ComputeHash($bytes)
        return (($hash | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 8)
    }
    finally {
        $sha.Dispose()
    }
}

function Get-MpcvrProfilePath {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [string]$ProfileRoot
    )

    if ([string]::IsNullOrWhiteSpace($Name)) {
        throw 'A profile name is required.'
    }

    $root = Get-MpcvrProfileRoot -ProfileRoot $ProfileRoot
    $safeName = [regex]::Replace($Name.Trim(), '[^\p{L}\p{Nd}._ -]+', '_').Trim(' ', '.')
    if ([string]::IsNullOrWhiteSpace($safeName)) {
        $safeName = 'profile'
    }
    if ($safeName.Length -gt 80) {
        $safeName = $safeName.Substring(0, 80).TrimEnd()
    }

    $hash = Get-MpcvrProfileNameHash -Name $Name
    return Join-Path $root ("{0}--{1}.json" -f $safeName, $hash)
}

function New-MpcvrProfile {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [ValidateSet('Automatic', 'Guided', 'Advanced')]
        [string]$Mode = 'Automatic',
        [ValidateSet('up-to-30', 'over-30-to-60', 'custom')]
        [string]$SourceFrameRateClass = 'up-to-30',
        [switch]$Locked
    )

    $targetFps = if ($SourceFrameRateClass -eq 'up-to-30') { 60.0 } elseif ($SourceFrameRateClass -eq 'over-30-to-60') { 120.0 } else { $null }

    return [pscustomobject]@{
        profileVersion = 1
        name = $Name.Trim()
        mode = $Mode.ToLowerInvariant()
        locked = [bool]$Locked
        machineFingerprint = [pscustomobject]@{
            gpuName = $null
            gpuPnpDeviceId = $null
            driverVersion = $null
            displayWidth = $null
            displayHeight = $null
            displayRefreshHz = $null
        }
        conditions = [pscustomobject]@{
            sourceFrameRateClass = $SourceFrameRateClass
            sourceWidthMax = $null
            sourceHeightMax = $null
            outputWidth = $null
            outputHeight = $null
            targetOutputFps = $targetFps
        }
        maxine = [pscustomobject]@{
            enabled = $false
            operation = 'upscale'
            quality = 'low'
            scaleLimit = 2.0
            oversampling = $false
            denoise = $false
            deblur = $false
            gpuSelection = 'auto'
        }
        frameInterpolation = [pscustomobject]@{
            enabled = $false
            sourceResolutionLimit = '1080p'
            maxOutputFps = if ($null -eq $targetFps) { 120.0 } else { $targetFps }
            runtimePath = $null
        }
        fallback = [pscustomobject]@{
            strategy = 'ask-user'
            showWarning = $true
            allowOverride = $true
        }
        calibration = [pscustomobject]@{
            capturedAtUtc = $null
            durationSeconds = $null
            maxineMilliseconds = $null
            frameInterpolationMilliseconds = $null
            totalMilliseconds = $null
            timingHeadroomMilliseconds = $null
            droppedFrames = $null
            failedFrames = $null
            pacingStable = $null
        }
    }
}

function Test-MpcvrProfile {
    param(
        [Parameter(Mandatory)]
        [object]$Profile
    )

    $errors = @()
    $required = @('profileVersion', 'name', 'mode', 'locked', 'conditions', 'maxine', 'frameInterpolation', 'fallback')
    $propertyNames = @($Profile.PSObject.Properties.Name)
    foreach ($propertyName in $required) {
        if ($propertyNames -notcontains $propertyName) {
            $errors += "Missing required property: $propertyName"
        }
    }

    if ($errors.Count -eq 0) {
        if ([int]$Profile.profileVersion -ne 1) {
            $errors += 'profileVersion must be 1.'
        }
        if ([string]::IsNullOrWhiteSpace([string]$Profile.name)) {
            $errors += 'name must not be empty.'
        }
        if ([string]$Profile.mode -notin @('automatic', 'guided', 'advanced')) {
            $errors += 'mode must be automatic, guided, or advanced.'
        }
        if ($Profile.locked -isnot [bool]) {
            $errors += 'locked must be a boolean.'
        }

        $conditionProperties = @($Profile.conditions.PSObject.Properties.Name)
        if ($conditionProperties -notcontains 'sourceFrameRateClass' -or
            [string]$Profile.conditions.sourceFrameRateClass -notin @('up-to-30', 'over-30-to-60', 'custom')) {
            $errors += 'conditions.sourceFrameRateClass is invalid.'
        }

        $maxineRequired = @('enabled', 'quality', 'scaleLimit', 'denoise', 'deblur', 'gpuSelection')
        foreach ($propertyName in $maxineRequired) {
            if (@($Profile.maxine.PSObject.Properties.Name) -notcontains $propertyName) {
                $errors += "Missing Maxine property: $propertyName"
            }
        }
        if ([string]$Profile.maxine.quality -notin @('low', 'medium', 'high', 'custom')) {
            $errors += 'maxine.quality is invalid.'
        }
        $scaleLimit = [double]$Profile.maxine.scaleLimit
        if ($scaleLimit -lt 1.0 -or $scaleLimit -gt 4.0) {
            $errors += 'maxine.scaleLimit must be between 1 and 4.'
        }

        $frucRequired = @('enabled', 'sourceResolutionLimit', 'maxOutputFps')
        foreach ($propertyName in $frucRequired) {
            if (@($Profile.frameInterpolation.PSObject.Properties.Name) -notcontains $propertyName) {
                $errors += "Missing frame-interpolation property: $propertyName"
            }
        }
        if ([string]$Profile.frameInterpolation.sourceResolutionLimit -notin @('720p', '1080p', '1440p', '2160p', 'custom')) {
            $errors += 'frameInterpolation.sourceResolutionLimit is invalid.'
        }
        if ([double]$Profile.frameInterpolation.maxOutputFps -lt 1.0) {
            $errors += 'frameInterpolation.maxOutputFps must be at least 1.'
        }

        if ([string]$Profile.fallback.strategy -notin @(
            'reduce-maxine-quality',
            'reduce-maxine-scale',
            'disable-maxine',
            'disable-interpolation',
            'ask-user',
            'none'
        )) {
            $errors += 'fallback.strategy is invalid.'
        }
    }

    return [pscustomobject]@{
        Valid = $errors.Count -eq 0
        Errors = $errors
    }
}

function Read-MpcvrProfile {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [switch]$AllowInvalid
    )

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $profile = Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json
    $validation = Test-MpcvrProfile -Profile $profile
    if (-not $validation.Valid -and -not $AllowInvalid) {
        throw ('Invalid profile {0}: {1}' -f $resolved, ($validation.Errors -join '; '))
    }
    return $profile
}

function Save-MpcvrProfile {
    param(
        [Parameter(Mandatory)]
        [object]$Profile,
        [string]$ProfileRoot,
        [switch]$Force,
        [switch]$AllowLockedOverwrite
    )

    $validation = Test-MpcvrProfile -Profile $Profile
    if (-not $validation.Valid) {
        throw ('Profile validation failed: {0}' -f ($validation.Errors -join '; '))
    }

    $root = Get-MpcvrProfileRoot -ProfileRoot $ProfileRoot
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $path = Get-MpcvrProfilePath -Name ([string]$Profile.name) -ProfileRoot $root

    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $existing = Read-MpcvrProfile -Path $path
        if ([bool]$existing.locked -and -not $AllowLockedOverwrite) {
            throw "Profile '$($existing.name)' is locked and cannot be overwritten without explicit permission."
        }
        if (-not $Force) {
            throw "Profile '$($existing.name)' already exists. Use Force to replace it."
        }
    }

    $Profile | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $path -Encoding UTF8
    return $path
}

function Get-MpcvrProfiles {
    param(
        [string]$ProfileRoot
    )

    $root = Get-MpcvrProfileRoot -ProfileRoot $ProfileRoot
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        return @()
    }

    $results = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $root -Filter '*.json' -File | Sort-Object Name)) {
        try {
            $profile = Read-MpcvrProfile -Path $file.FullName
            $results += [pscustomobject]@{
                Name = [string]$profile.name
                Mode = [string]$profile.mode
                Locked = [bool]$profile.locked
                SourceFrameRateClass = [string]$profile.conditions.sourceFrameRateClass
                Path = $file.FullName
                Valid = $true
                Errors = @()
                Profile = $profile
            }
        }
        catch {
            $results += [pscustomobject]@{
                Name = $file.BaseName
                Mode = $null
                Locked = $null
                SourceFrameRateClass = $null
                Path = $file.FullName
                Valid = $false
                Errors = @($_.Exception.Message)
                Profile = $null
            }
        }
    }
    return $results
}

function Find-MpcvrProfile {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [string]$ProfileRoot
    )

    $path = Get-MpcvrProfilePath -Name $Name -ProfileRoot $ProfileRoot
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Profile was not found: $Name"
    }
    return [pscustomobject]@{
        Path = $path
        Profile = Read-MpcvrProfile -Path $path
    }
}

function Set-MpcvrProfileLock {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [Parameter(Mandatory)]
        [bool]$Locked,
        [string]$ProfileRoot
    )

    $found = Find-MpcvrProfile -Name $Name -ProfileRoot $ProfileRoot
    $found.Profile.locked = $Locked
    return Save-MpcvrProfile `
        -Profile $found.Profile `
        -ProfileRoot $ProfileRoot `
        -Force `
        -AllowLockedOverwrite
}

function Remove-MpcvrProfile {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [string]$ProfileRoot,
        [switch]$AllowLockedDelete
    )

    $found = Find-MpcvrProfile -Name $Name -ProfileRoot $ProfileRoot
    if ([bool]$found.Profile.locked -and -not $AllowLockedDelete) {
        throw "Profile '$Name' is locked and cannot be deleted without explicit permission."
    }
    Remove-Item -LiteralPath $found.Path -Force
}

function Export-MpcvrProfile {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [Parameter(Mandatory)]
        [string]$Destination,
        [string]$ProfileRoot,
        [switch]$Force
    )

    $found = Find-MpcvrProfile -Name $Name -ProfileRoot $ProfileRoot
    $destinationPath = [IO.Path]::GetFullPath($Destination)
    if (Test-Path -LiteralPath $destinationPath -PathType Container) {
        $destinationPath = Join-Path $destinationPath ([IO.Path]::GetFileName($found.Path))
    }
    if ((Test-Path -LiteralPath $destinationPath) -and -not $Force) {
        throw "Export destination already exists: $destinationPath"
    }
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destinationPath)) -Force | Out-Null
    Copy-Item -LiteralPath $found.Path -Destination $destinationPath -Force
    return $destinationPath
}

function Import-MpcvrProfile {
    param(
        [Parameter(Mandatory)]
        [string]$Source,
        [string]$Name,
        [string]$ProfileRoot,
        [switch]$Force,
        [switch]$AllowLockedOverwrite
    )

    $profile = Read-MpcvrProfile -Path $Source
    if (-not [string]::IsNullOrWhiteSpace($Name)) {
        $profile.name = $Name.Trim()
    }
    return Save-MpcvrProfile `
        -Profile $profile `
        -ProfileRoot $ProfileRoot `
        -Force:$Force `
        -AllowLockedOverwrite:$AllowLockedOverwrite
}

function Restore-MpcvrFactoryProfiles {
    param(
        [string]$ProfileRoot,
        [switch]$Force
    )

    $templates = @(
        New-MpcvrProfile -Name 'Automatic - up to 30 fps (uncalibrated)' -Mode Automatic -SourceFrameRateClass 'up-to-30',
        New-MpcvrProfile -Name 'Automatic - 30 to 60 fps (uncalibrated)' -Mode Automatic -SourceFrameRateClass 'over-30-to-60'
    )

    $saved = @()
    foreach ($profile in $templates) {
        try {
            $saved += Save-MpcvrProfile -Profile $profile -ProfileRoot $ProfileRoot -Force:$Force
        }
        catch {
            if ($_.Exception.Message -match 'locked') {
                Write-Warning $_.Exception.Message
                continue
            }
            throw
        }
    }
    return $saved
}

Export-ModuleMember -Function @(
    'Get-MpcvrProfileRoot',
    'Get-MpcvrProfilePath',
    'New-MpcvrProfile',
    'Test-MpcvrProfile',
    'Read-MpcvrProfile',
    'Save-MpcvrProfile',
    'Get-MpcvrProfiles',
    'Find-MpcvrProfile',
    'Set-MpcvrProfileLock',
    'Remove-MpcvrProfile',
    'Export-MpcvrProfile',
    'Import-MpcvrProfile',
    'Restore-MpcvrFactoryProfiles'
)
