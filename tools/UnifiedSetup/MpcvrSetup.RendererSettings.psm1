#requires -Version 5.1

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Get-MpcvrRendererSettingDefinitions {
    return @(
        [pscustomobject]@{ Name = 'MaxineOperation'; Default = 0 },
        [pscustomobject]@{ Name = 'MaxineSourceMode'; Default = 0 },
        [pscustomobject]@{ Name = 'MaxineQuality'; Default = 3 },
        [pscustomobject]@{ Name = 'MaxineScale'; Default = 0 },
        [pscustomobject]@{ Name = 'MaxineOversample'; Default = 100 },
        [pscustomobject]@{ Name = 'MaxineSourceLimit'; Default = 3 },
        [pscustomobject]@{ Name = 'MaxineDenoise'; Default = 0 },
        [pscustomobject]@{ Name = 'MaxineDeblur'; Default = 0 },
        [pscustomobject]@{ Name = 'MaxinePipeline'; Default = 0 },
        [pscustomobject]@{ Name = 'MaxineGPU'; Default = -1 },
        [pscustomobject]@{ Name = 'MaxineAutoBitrate'; Default = 20 },
        [pscustomobject]@{ Name = 'FrameInterpolationMode'; Default = 0 },
        [pscustomobject]@{ Name = 'FrameInterpolationSourceLimit'; Default = 1 },
        [pscustomobject]@{ Name = 'FrameInterpolationMaxOutput'; Default = 60 },
        [pscustomobject]@{ Name = 'FrameInterpolationGPU'; Default = -1 },
        [pscustomobject]@{ Name = 'FrameInterpolationFallback'; Default = 1 }
    )
}

function Get-MpcvrObjectProperty {
    param(
        [object]$Object,
        [Parameter(Mandatory)]
        [string]$Name,
        $Default = $null
    )

    if ($null -eq $Object) {
        return $Default
    }
    $property = $Object.PSObject.Properties |
        Where-Object { $_.Name -ieq $Name } |
        Select-Object -First 1
    if ($null -eq $property) {
        return $Default
    }
    return $property.Value
}

function ConvertTo-MpcvrBooleanDword {
    param($Value)

    if ($Value -is [bool]) {
        return $(if ($Value) { 1 } else { 0 })
    }
    if ($null -eq $Value) {
        return 0
    }
    $text = ([string]$Value).Trim().ToLowerInvariant()
    if ($text -in @('1', 'true', 'yes', 'on', 'enabled')) {
        return 1
    }
    if ($text -in @('0', 'false', 'no', 'off', 'disabled')) {
        return 0
    }
    throw "Cannot convert '$Value' to a renderer boolean DWORD."
}

function ConvertTo-MpcvrMappedValue {
    param(
        $Value,
        [Parameter(Mandatory)]
        [hashtable]$Map,
        [Parameter(Mandatory)]
        [string]$SettingName
    )

    if ($Value -is [byte] -or $Value -is [int16] -or $Value -is [int32] -or
        $Value -is [int64] -or $Value -is [uint16] -or $Value -is [uint32]) {
        return [int64]$Value
    }
    $key = ([string]$Value).Trim().ToLowerInvariant()
    if ($Map.ContainsKey($key)) {
        return [int64]$Map[$key]
    }
    throw "Unsupported $SettingName value: $Value"
}

function ConvertTo-MpcvrScaleValue {
    param($Value)

    if ($null -eq $Value) {
        return 0
    }
    $text = ([string]$Value).Trim().ToLowerInvariant()
    $named = @{
        'match-output' = 0
        'match output' = 0
        'auto' = 0
        '1.333' = 133
        '4/3' = 133
        '4:3' = 133
        '1.5' = 150
        '2' = 200
        '2x' = 200
        '3' = 300
        '3x' = 300
        '4' = 400
        '4x' = 400
    }
    if ($named.ContainsKey($text)) {
        return [int]$named[$text]
    }

    try {
        $numeric = [double]$Value
    }
    catch {
        throw "Unsupported Maxine scale value: $Value"
    }
    if ($numeric -le 1.0) {
        return 0
    }
    $choices = @(133, 150, 200, 300, 400)
    $target = $numeric * 100.0
    return [int]($choices | Sort-Object { [math]::Abs($_ - $target) } | Select-Object -First 1)
}

function ConvertTo-MpcvrMaxOutputValue {
    param($Value)

    $numeric = [double]$Value
    foreach ($choice in @(60, 120, 240)) {
        if ($numeric -le $choice) {
            return $choice
        }
    }
    return 240
}

function ConvertTo-MpcvrFilterValue {
    param($Value)

    if ($Value -is [bool]) {
        return $(if ($Value) { 1 } else { 0 })
    }
    return ConvertTo-MpcvrMappedValue -Value $Value -SettingName 'Maxine filter' -Map @{
        'off' = 0
        'disabled' = 0
        'low' = 1
        'medium' = 2
        'high' = 3
        'ultra' = 4
    }
}

function ConvertTo-MpcvrRendererSettings {
    param(
        [Parameter(Mandatory)]
        [object]$Profile
    )

    $settings = [ordered]@{}
    $exactSettings = Get-MpcvrObjectProperty -Object $Profile -Name 'rendererSettings'
    if ($null -ne $exactSettings) {
        foreach ($definition in Get-MpcvrRendererSettingDefinitions) {
            $value = Get-MpcvrObjectProperty -Object $exactSettings -Name $definition.Name
            if ($null -ne $value) {
                $settings[$definition.Name] = [int64]$value
            }
        }
        if ($settings.Count -eq 0) {
            throw 'rendererSettings exists but contains no recognized MPCVR settings.'
        }
        return $settings
    }

    $maxine = Get-MpcvrObjectProperty -Object $Profile -Name 'maxine'
    $fruc = Get-MpcvrObjectProperty -Object $Profile -Name 'frameInterpolation'

    if ($null -ne $maxine) {
        $maxineEnabled = (ConvertTo-MpcvrBooleanDword -Value (Get-MpcvrObjectProperty -Object $maxine -Name 'enabled' -Default $false)) -eq 1
        $operation = if ($maxineEnabled) {
            ConvertTo-MpcvrMappedValue `
                -Value (Get-MpcvrObjectProperty -Object $maxine -Name 'operation' -Default 'upscale') `
                -SettingName 'Maxine operation' `
                -Map @{ 'disabled' = 0; 'upscale' = 1; 'denoise' = 2; 'deblur' = 3 }
        }
        else { 0 }
        $settings['MaxineOperation'] = $operation
        $settings['MaxineSourceMode'] = ConvertTo-MpcvrMappedValue `
            -Value (Get-MpcvrObjectProperty -Object $maxine -Name 'sourceMode' -Default 'auto') `
            -SettingName 'Maxine source mode' `
            -Map @{ 'auto' = 0; 'standard' = 1; 'high-bitrate' = 2; 'high bitrate' = 2; 'bicubic' = 3 }
        $settings['MaxineQuality'] = ConvertTo-MpcvrMappedValue `
            -Value (Get-MpcvrObjectProperty -Object $maxine -Name 'quality' -Default 'high') `
            -SettingName 'Maxine quality' `
            -Map @{ 'low' = 1; 'medium' = 2; 'high' = 3; 'ultra' = 4 }
        $settings['MaxineScale'] = ConvertTo-MpcvrScaleValue `
            -Value (Get-MpcvrObjectProperty -Object $maxine -Name 'scaleLimit' -Default 'match-output')

        $oversampling = Get-MpcvrObjectProperty -Object $maxine -Name 'oversampling' -Default $false
        if ((ConvertTo-MpcvrBooleanDword -Value $oversampling) -eq 0) {
            $settings['MaxineOversample'] = 100
        }
        else {
            $settings['MaxineOversample'] = ConvertTo-MpcvrScaleValue `
                -Value (Get-MpcvrObjectProperty -Object $maxine -Name 'oversamplingScale' -Default 1.333)
            if ($settings['MaxineOversample'] -eq 0) {
                $settings['MaxineOversample'] = 133
            }
        }

        $sourceLimit = Get-MpcvrObjectProperty -Object $maxine -Name 'sourceResolutionLimit'
        if ($null -ne $sourceLimit) {
            $settings['MaxineSourceLimit'] = ConvertTo-MpcvrMappedValue `
                -Value $sourceLimit `
                -SettingName 'Maxine source resolution limit' `
                -Map @{ 'disabled' = 0; 'sd' = 1; '720p' = 2; '1080p' = 3; '1440p' = 4 }
        }
        $settings['MaxineDenoise'] = ConvertTo-MpcvrFilterValue `
            -Value (Get-MpcvrObjectProperty -Object $maxine -Name 'denoise' -Default 'off')
        $settings['MaxineDeblur'] = ConvertTo-MpcvrFilterValue `
            -Value (Get-MpcvrObjectProperty -Object $maxine -Name 'deblur' -Default 'off')
        $settings['MaxinePipeline'] = ConvertTo-MpcvrMappedValue `
            -Value (Get-MpcvrObjectProperty -Object $maxine -Name 'pipeline' -Default 'upscale-denoise-deblur') `
            -SettingName 'Maxine pipeline' `
            -Map @{
                'upscale-denoise-deblur' = 0
                'upscale-deblur-denoise' = 1
                'denoise-deblur-upscale' = 2
                'deblur-denoise-upscale' = 3
                'denoise-upscale-deblur' = 4
                'deblur-upscale-denoise' = 5
            }
        $gpuSelection = Get-MpcvrObjectProperty -Object $maxine -Name 'gpuSelection' -Default 'auto'
        $settings['MaxineGPU'] = if (([string]$gpuSelection).Trim().ToLowerInvariant() -eq 'auto') { -1 } else { [int]$gpuSelection }
        $settings['MaxineAutoBitrate'] = [int](Get-MpcvrObjectProperty -Object $maxine -Name 'autoBitrate' -Default 20)
    }

    if ($null -ne $fruc) {
        $frucEnabled = (ConvertTo-MpcvrBooleanDword -Value (Get-MpcvrObjectProperty -Object $fruc -Name 'enabled' -Default $false)) -eq 1
        $settings['FrameInterpolationMode'] = if ($frucEnabled) { 1 } else { 0 }
        $settings['FrameInterpolationSourceLimit'] = ConvertTo-MpcvrMappedValue `
            -Value (Get-MpcvrObjectProperty -Object $fruc -Name 'sourceResolutionLimit' -Default '1080p') `
            -SettingName 'frame interpolation source limit' `
            -Map @{ '720p' = 0; '1080p' = 1; '1440p' = 2; '2160p' = 3 }
        $settings['FrameInterpolationMaxOutput'] = ConvertTo-MpcvrMaxOutputValue `
            -Value (Get-MpcvrObjectProperty -Object $fruc -Name 'maxOutputFps' -Default 60)
        $frucGpu = Get-MpcvrObjectProperty -Object $fruc -Name 'gpuSelection' -Default 'auto'
        $settings['FrameInterpolationGPU'] = if (([string]$frucGpu).Trim().ToLowerInvariant() -eq 'auto') { -1 } else { [int]$frucGpu }
        $settings['FrameInterpolationFallback'] = ConvertTo-MpcvrBooleanDword `
            -Value (Get-MpcvrObjectProperty -Object $fruc -Name 'failureFallback' -Default $true)
    }

    if ($settings.Count -eq 0) {
        throw 'The profile contains neither rendererSettings nor semantic Maxine/frame-interpolation settings.'
    }
    return $settings
}

function Get-MpcvrRendererSettings {
    param(
        [string]$RegistryPath = 'HKCU:\Software\MPCVideoRenderer'
    )

    $definitions = @(Get-MpcvrRendererSettingDefinitions)
    $valueNames = @()
    $key = $null
    if (Test-Path -LiteralPath $RegistryPath) {
        $key = Get-Item -LiteralPath $RegistryPath
        $valueNames = @($key.GetValueNames())
    }

    $values = @()
    $settings = [ordered]@{}
    foreach ($definition in $definitions) {
        $exists = $valueNames -contains $definition.Name
        $value = if ($exists) { [int64]$key.GetValue($definition.Name) } else { [int64]$definition.Default }
        $settings[$definition.Name] = $value
        $values += [pscustomobject]@{
            Name = $definition.Name
            Exists = $exists
            Value = $value
            EffectiveValue = $value
            DefaultValue = [int64]$definition.Default
        }
    }

    return [pscustomobject]@{
        SchemaVersion = 1
        RegistryPath = $RegistryPath
        CapturedAtUtc = [DateTime]::UtcNow.ToString('o')
        Values = $values
        Settings = [pscustomobject]$settings
    }
}

function Compare-MpcvrRendererSettings {
    param(
        [Parameter(Mandatory)]
        [object]$Current,
        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$Desired
    )

    $changes = @()
    foreach ($name in $Desired.Keys) {
        $currentProperty = $Current.Settings.PSObject.Properties |
            Where-Object { $_.Name -ieq $name } |
            Select-Object -First 1
        if ($null -eq $currentProperty) {
            throw "Unknown renderer setting: $name"
        }
        $before = [int64]$currentProperty.Value
        $after = [int64]$Desired[$name]
        if ($before -ne $after) {
            $changes += [pscustomobject]@{
                Name = [string]$name
                Before = $before
                After = $after
            }
        }
    }
    return $changes
}

function Save-MpcvrRendererSettingsBackup {
    param(
        [Parameter(Mandatory)]
        [object]$Snapshot,
        [Parameter(Mandatory)]
        [string]$Path
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($fullPath)
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $Snapshot | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $fullPath -Encoding UTF8
    return $fullPath
}

function Set-MpcvrRegistryDword {
    param(
        [Parameter(Mandatory)]
        [string]$RegistryPath,
        [Parameter(Mandatory)]
        [string]$Name,
        [Parameter(Mandatory)]
        [int64]$Value
    )

    if ($Value -lt [int32]::MinValue -or $Value -gt [uint32]::MaxValue) {
        throw "Renderer DWORD '$Name' is outside the supported range: $Value"
    }
    if (-not (Test-Path -LiteralPath $RegistryPath)) {
        New-Item -Path $RegistryPath -Force | Out-Null
    }
    $storedValue = if ($Value -gt [int32]::MaxValue) { [uint32]$Value } else { [int32]$Value }
    New-ItemProperty `
        -LiteralPath $RegistryPath `
        -Name $Name `
        -Value $storedValue `
        -PropertyType DWord `
        -Force | Out-Null
}

function Restore-MpcvrRendererSettingsBackup {
    param(
        [Parameter(Mandatory)]
        [string]$BackupPath,
        [string]$RegistryPath,
        [switch]$AllowPlayerRunning
    )

    if (-not $AllowPlayerRunning -and
        (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue)) {
        throw 'Close MPC-HC before restoring renderer settings.'
    }

    $backup = Get-Content -LiteralPath (Resolve-Path -LiteralPath $BackupPath).Path -Raw | ConvertFrom-Json
    if ([int]$backup.SchemaVersion -ne 1) {
        throw "Unsupported renderer-settings backup schema: $($backup.SchemaVersion)"
    }
    if ([string]::IsNullOrWhiteSpace($RegistryPath)) {
        $RegistryPath = [string]$backup.RegistryPath
    }
    if ([string]::IsNullOrWhiteSpace($RegistryPath)) {
        throw 'The renderer-settings backup does not contain a registry path.'
    }

    foreach ($entry in @($backup.Values)) {
        if ([bool]$entry.Exists) {
            Set-MpcvrRegistryDword -RegistryPath $RegistryPath -Name ([string]$entry.Name) -Value ([int64]$entry.Value)
        }
        elseif (Test-Path -LiteralPath $RegistryPath) {
            Remove-ItemProperty -LiteralPath $RegistryPath -Name ([string]$entry.Name) -ErrorAction SilentlyContinue
        }
    }
    return Get-MpcvrRendererSettings -RegistryPath $RegistryPath
}

function Set-MpcvrRendererSettings {
    param(
        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$Settings,
        [string]$RegistryPath = 'HKCU:\Software\MPCVideoRenderer',
        [string]$BackupPath,
        [switch]$DryRun,
        [switch]$AllowPlayerRunning
    )

    if (-not $AllowPlayerRunning -and
        (Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue)) {
        throw 'Close MPC-HC before applying renderer settings.'
    }

    $definitions = @{}
    foreach ($definition in Get-MpcvrRendererSettingDefinitions) {
        $definitions[$definition.Name] = $true
    }
    foreach ($name in $Settings.Keys) {
        if (-not $definitions.ContainsKey([string]$name)) {
            throw "Unknown renderer setting: $name"
        }
    }

    $before = Get-MpcvrRendererSettings -RegistryPath $RegistryPath
    $changes = @(Compare-MpcvrRendererSettings -Current $before -Desired $Settings)
    if ($DryRun -or $changes.Count -eq 0) {
        return [pscustomobject]@{
            Applied = $false
            DryRun = [bool]$DryRun
            RegistryPath = $RegistryPath
            BackupPath = $null
            Changes = $changes
            Before = $before
            After = $before
        }
    }

    if ([string]::IsNullOrWhiteSpace($BackupPath)) {
        $backupRoot = Join-Path $env:LOCALAPPDATA 'MPCVR Unified Setup\settings-backups'
        $BackupPath = Join-Path $backupRoot ('renderer-settings-{0}.json' -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    }
    $savedBackup = Save-MpcvrRendererSettingsBackup -Snapshot $before -Path $BackupPath

    try {
        foreach ($change in $changes) {
            Set-MpcvrRegistryDword `
                -RegistryPath $RegistryPath `
                -Name $change.Name `
                -Value $change.After
        }
        $after = Get-MpcvrRendererSettings -RegistryPath $RegistryPath
        $remaining = @(Compare-MpcvrRendererSettings -Current $after -Desired $Settings)
        if ($remaining.Count -gt 0) {
            throw ('Renderer settings verification failed for: {0}' -f (($remaining | ForEach-Object { $_.Name }) -join ', '))
        }
    }
    catch {
        try {
            [void](Restore-MpcvrRendererSettingsBackup `
                -BackupPath $savedBackup `
                -RegistryPath $RegistryPath `
                -AllowPlayerRunning)
        }
        catch {
            throw "Renderer settings apply failed and rollback also failed: $($_.Exception.Message)"
        }
        throw
    }

    return [pscustomobject]@{
        Applied = $true
        DryRun = $false
        RegistryPath = $RegistryPath
        BackupPath = $savedBackup
        Changes = $changes
        Before = $before
        After = $after
    }
}

Export-ModuleMember -Function @(
    'Get-MpcvrRendererSettingDefinitions',
    'ConvertTo-MpcvrRendererSettings',
    'Get-MpcvrRendererSettings',
    'Compare-MpcvrRendererSettings',
    'Save-MpcvrRendererSettingsBackup',
    'Restore-MpcvrRendererSettingsBackup',
    'Set-MpcvrRendererSettings'
)
