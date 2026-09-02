#requires -Version 5.1

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Test-MpcvrWindows {
    return $env:OS -eq 'Windows_NT'
}

function Get-MpcvrNvidiaSmiPath {
    $command = Get-Command 'nvidia-smi.exe' -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $candidates = @(
        (Join-Path $env:ProgramFiles 'NVIDIA Corporation\NVSMI\nvidia-smi.exe'),
        (Join-Path $env:SystemRoot 'System32\nvidia-smi.exe')
    )

    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return $candidate
        }
    }

    return $null
}

function Get-MpcvrNvidiaSmiData {
    $nvidiaSmi = Get-MpcvrNvidiaSmiPath
    if ([string]::IsNullOrWhiteSpace($nvidiaSmi)) {
        return @()
    }

    try {
        $lines = & $nvidiaSmi `
            '--query-gpu=name,driver_version,memory.total,pci.device_id,pstate' `
            '--format=csv,noheader,nounits' 2>$null
        if ($LASTEXITCODE -ne 0 -or $null -eq $lines) {
            return @()
        }

        $rows = @($lines | ConvertFrom-Csv -Header 'Name', 'DriverVersion', 'MemoryMiB', 'PciDeviceId', 'PerformanceState')
        return @($rows | ForEach-Object {
            [pscustomobject]@{
                Name = ([string]$_.Name).Trim()
                DriverVersion = ([string]$_.DriverVersion).Trim()
                MemoryMiB = [int](([string]$_.MemoryMiB).Trim())
                PciDeviceId = ([string]$_.PciDeviceId).Trim()
                PerformanceState = ([string]$_.PerformanceState).Trim()
                Source = 'nvidia-smi'
            }
        })
    }
    catch {
        return @()
    }
}

function Get-MpcvrGpuProfile {
    $smiRows = @(Get-MpcvrNvidiaSmiData)
    $controllers = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match 'NVIDIA' })

    if ($smiRows.Count -gt 0) {
        return @($smiRows | ForEach-Object {
            $row = $_
            $controller = $controllers |
                Where-Object { $_.Name -eq $row.Name } |
                Select-Object -First 1

            [pscustomobject]@{
                Name = $row.Name
                DriverVersion = $row.DriverVersion
                MemoryMiB = $row.MemoryMiB
                PciDeviceId = $row.PciDeviceId
                PnpDeviceId = if ($controller) { [string]$controller.PNPDeviceID } else { $null }
                CurrentWidth = if ($controller) { [int]$controller.CurrentHorizontalResolution } else { 0 }
                CurrentHeight = if ($controller) { [int]$controller.CurrentVerticalResolution } else { 0 }
                CurrentRefreshHz = if ($controller) { [int]$controller.CurrentRefreshRate } else { 0 }
                PerformanceState = $row.PerformanceState
                DetectionSource = $row.Source
            }
        })
    }

    return @($controllers | ForEach-Object {
        $memoryMiB = 0
        if ($null -ne $_.AdapterRAM) {
            $memoryMiB = [int]([uint64]$_.AdapterRAM / 1MB)
        }

        [pscustomobject]@{
            Name = [string]$_.Name
            DriverVersion = [string]$_.DriverVersion
            MemoryMiB = $memoryMiB
            PciDeviceId = $null
            PnpDeviceId = [string]$_.PNPDeviceID
            CurrentWidth = [int]$_.CurrentHorizontalResolution
            CurrentHeight = [int]$_.CurrentVerticalResolution
            CurrentRefreshHz = [int]$_.CurrentRefreshRate
            PerformanceState = $null
            DetectionSource = 'Win32_VideoController'
        }
    })
}

function Get-MpcvrDisplayProfile {
    $monitors = @(Get-CimInstance -ClassName Win32_DesktopMonitor -ErrorAction SilentlyContinue)
    return @($monitors | ForEach-Object {
        [pscustomobject]@{
            Name = if ([string]::IsNullOrWhiteSpace([string]$_.Name)) { [string]$_.MonitorType } else { [string]$_.Name }
            PnpDeviceId = [string]$_.PNPDeviceID
            ScreenWidth = [int]$_.ScreenWidth
            ScreenHeight = [int]$_.ScreenHeight
            Status = [string]$_.Status
        }
    })
}

function Test-MpcvrRequiredFiles {
    param(
        [string]$Root,
        [string[]]$RequiredFiles
    )

    if ([string]::IsNullOrWhiteSpace($Root) -or
        -not (Test-Path -LiteralPath $Root -PathType Container)) {
        return [pscustomobject]@{
            Installed = $false
            Path = $Root
            MissingFiles = @($RequiredFiles)
        }
    }

    $missing = @($RequiredFiles | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $Root $_) -PathType Leaf)
    })

    return [pscustomobject]@{
        Installed = $missing.Count -eq 0
        Path = $Root
        MissingFiles = $missing
    }
}

function Get-MpcvrRuntimeStatus {
    $maxineDefault = Join-Path $env:LOCALAPPDATA 'MPCVR Maxine Runtime\nvvfx\libs'
    $maxinePath = [Environment]::GetEnvironmentVariable('NV_VIDEO_EFFECTS_PATH', 'User')
    if ([string]::IsNullOrWhiteSpace($maxinePath)) {
        $maxinePath = $env:NV_VIDEO_EFFECTS_PATH
    }
    if ([string]::IsNullOrWhiteSpace($maxinePath)) {
        $maxinePath = $maxineDefault
    }

    $frucDefault = Join-Path $env:LOCALAPPDATA 'MPCVR NvOFFRUC Runtime'
    $frucPath = [Environment]::GetEnvironmentVariable('NV_OFFRUC_PATH', 'User')
    if ([string]::IsNullOrWhiteSpace($frucPath)) {
        $frucPath = $env:NV_OFFRUC_PATH
    }
    if ([string]::IsNullOrWhiteSpace($frucPath)) {
        $frucPath = $frucDefault
    }

    $maxine = Test-MpcvrRequiredFiles -Root $maxinePath -RequiredFiles @(
        'NVCVImage.dll',
        'NVVideoEffects.dll',
        'nvngxruntime.dll',
        'nvngx_vsr.dll',
        'nvVFXVideoSuperRes.dll'
    )
    $fruc = Test-MpcvrRequiredFiles -Root $frucPath -RequiredFiles @(
        'NvOFFRUC.dll',
        'cudart64_110.dll'
    )

    return [pscustomobject]@{
        Maxine = $maxine
        NvOFFRUC = $fruc
    }
}

function Get-MpcvrPlayerTargets {
    $targets = @(
        [pscustomobject]@{
            Name = 'K-Lite MPC-HC x64'
            Architecture = 'x64'
            RendererPath = 'C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax'
        },
        [pscustomobject]@{
            Name = 'K-Lite MPC-HC x86'
            Architecture = 'x86'
            RendererPath = 'C:\Program Files (x86)\K-Lite Codec Pack\Filters\MPCVR\MpcVideoRenderer.ax'
        }
    )

    return @($targets | ForEach-Object {
        $renderer = $_.RendererPath
        $directory = [IO.Path]::GetDirectoryName($renderer)
        [pscustomobject]@{
            Name = $_.Name
            Architecture = $_.Architecture
            RendererPath = $renderer
            DirectoryExists = Test-Path -LiteralPath $directory -PathType Container
            RendererExists = Test-Path -LiteralPath $renderer -PathType Leaf
            RendererVersion = if (Test-Path -LiteralPath $renderer -PathType Leaf) {
                (Get-Item -LiteralPath $renderer).VersionInfo.FileVersion
            }
            else {
                $null
            }
        }
    })
}

function Get-MpcvrSystemProfile {
    if (-not (Test-MpcvrWindows)) {
        throw 'MPCVR unified setup only supports Windows.'
    }

    $operatingSystem = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction SilentlyContinue
    $computerSystem = Get-CimInstance -ClassName Win32_ComputerSystem -ErrorAction SilentlyContinue

    return [pscustomobject]@{
        SchemaVersion = 1
        CapturedAtUtc = [DateTime]::UtcNow.ToString('o')
        Machine = [pscustomobject]@{
            ComputerName = $env:COMPUTERNAME
            Manufacturer = if ($computerSystem) { [string]$computerSystem.Manufacturer } else { $null }
            Model = if ($computerSystem) { [string]$computerSystem.Model } else { $null }
            OsCaption = if ($operatingSystem) { [string]$operatingSystem.Caption } else { $null }
            OsVersion = if ($operatingSystem) { [string]$operatingSystem.Version } else { $null }
            OsBuild = if ($operatingSystem) { [string]$operatingSystem.BuildNumber } else { $null }
            PowerShellVersion = $PSVersionTable.PSVersion.ToString()
        }
        Gpus = @(Get-MpcvrGpuProfile)
        Displays = @(Get-MpcvrDisplayProfile)
        Players = @(Get-MpcvrPlayerTargets)
        Runtimes = Get-MpcvrRuntimeStatus
    }
}

function Save-MpcvrSystemProfile {
    param(
        [Parameter(Mandatory)]
        [object]$Profile,
        [Parameter(Mandatory)]
        [string]$Path
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($fullPath)
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    $Profile | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $fullPath -Encoding UTF8
    return $fullPath
}

Export-ModuleMember -Function @(
    'Get-MpcvrGpuProfile',
    'Get-MpcvrDisplayProfile',
    'Get-MpcvrRuntimeStatus',
    'Get-MpcvrPlayerTargets',
    'Get-MpcvrSystemProfile',
    'Save-MpcvrSystemProfile'
)
