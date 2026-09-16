Set-StrictMode -Version 2.0

function ConvertTo-RifeArchitectureKey([int]$Major, [int]$Minor) {
    $key = '{0}.{1}' -f $Major, $Minor
    switch ($key) {
        '7.5'  { 'sm75'; break }
        '8.6'  { 'sm86'; break }
        '8.9'  { 'sm89'; break }
        '12.0' { 'sm120'; break }
        default { throw "Unsupported CUDA compute capability: $key" }
    }
}

function ConvertTo-RifeArchitectureKeyFromCapability([string]$ComputeCapability) {
    if ([string]::IsNullOrWhiteSpace($ComputeCapability)) {
        throw 'Unsupported CUDA compute capability: <blank>'
    }

    $value = $ComputeCapability.Trim()
    $match = [regex]::Match($value, '^(\d+)\.(\d+)$')
    if (-not $match.Success) {
        throw "Unsupported CUDA compute capability: $value"
    }

    ConvertTo-RifeArchitectureKey -Major ([int]$match.Groups[1].Value) -Minor ([int]$match.Groups[2].Value)
}

function Get-RifeBuilderResourceName([string]$Architecture) {
    if ($Architecture -notin @('sm75', 'sm86', 'sm89', 'sm120')) {
        throw "Unsupported RIFE architecture pack: $Architecture"
    }

    "nvinfer_builder_resource_${Architecture}_11.dll"
}

function Get-RifeExpectedArchitectures {
    @(
        [pscustomobject]@{
            ComputeCapability = '7.5'
            Key = 'sm75'
            BuilderResource = 'nvinfer_builder_resource_sm75_11.dll'
        },
        [pscustomobject]@{
            ComputeCapability = '8.6'
            Key = 'sm86'
            BuilderResource = 'nvinfer_builder_resource_sm86_11.dll'
        },
        [pscustomobject]@{
            ComputeCapability = '8.9'
            Key = 'sm89'
            BuilderResource = 'nvinfer_builder_resource_sm89_11.dll'
        },
        [pscustomobject]@{
            ComputeCapability = '12.0'
            Key = 'sm120'
            BuilderResource = 'nvinfer_builder_resource_sm120_11.dll'
        }
    )
}

function Get-RifeCommonRuntimeFileNames {
    @(
        'MPCVRRifeRuntime64.dll',
        'cudart64_12.dll',
        'nvinfer_11.dll',
        'nvonnxparser_11.dll',
        'nvinfer_builder_resource_ptx_11.dll',
        'BUILD-INFO.txt'
    )
}

function Get-RifeRuntimeArchiveNames {
    @(
        'MPCVR-RIFE-Common.zip',
        'MPCVR-RIFE-sm75.zip',
        'MPCVR-RIFE-sm86.zip',
        'MPCVR-RIFE-sm89.zip',
        'MPCVR-RIFE-sm120.zip'
    )
}

function Get-RifeGpuInventory([string]$GpuInventoryJson) {
    if (-not [string]::IsNullOrWhiteSpace($GpuInventoryJson)) {
        $json = $GpuInventoryJson
        $inventoryPathExists = $false
        try {
            $inventoryPathExists = Test-Path -LiteralPath $GpuInventoryJson -PathType Leaf -ErrorAction SilentlyContinue
        }
        catch {
            $inventoryPathExists = $false
        }
        if ($inventoryPathExists) {
            $json = Get-Content -LiteralPath $GpuInventoryJson -Raw
        }

        try {
            $parsed = $json | ConvertFrom-Json
        }
        catch {
            throw "GPU inventory JSON is invalid: $($_.Exception.Message)"
        }

        $inventory = @($parsed)
    }
    else {
        $command = Get-Command 'nvidia-smi.exe' -ErrorAction SilentlyContinue
        if (-not $command) {
            throw 'nvidia-smi.exe was not found. Install a supported NVIDIA driver or provide -GpuInventoryJson.'
        }

        $lines = @(& $command.Source '--query-gpu=name,compute_cap' '--format=csv,noheader,nounits' 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "nvidia-smi.exe failed while querying GPU compute capabilities: $($lines -join ' ')"
        }

        $inventory = @(
            foreach ($line in $lines) {
                $text = [string]$line
                if ([string]::IsNullOrWhiteSpace($text)) {
                    continue
                }

                $parts = $text -split ',', 2
                if ($parts.Count -ne 2) {
                    throw "Unexpected nvidia-smi GPU inventory row: $text"
                }

                [pscustomobject]@{
                    name = $parts[0].Trim()
                    computeCapability = $parts[1].Trim()
                }
            }
        )
    }

    if ($inventory.Count -eq 0) {
        throw 'No NVIDIA GPUs were found in the GPU inventory.'
    }

    @(
        foreach ($gpu in $inventory) {
            $nameProperty = $gpu.PSObject.Properties['name']
            $capabilityProperty = $gpu.PSObject.Properties['computeCapability']
            if (-not $nameProperty -or -not $capabilityProperty) {
                throw 'Each GPU inventory entry must contain name and computeCapability.'
            }

            $name = [string]$nameProperty.Value
            $capability = [string]$capabilityProperty.Value
            if ([string]::IsNullOrWhiteSpace($name)) {
                throw 'GPU inventory contains an entry with a blank name.'
            }

            # Validate every capability while normalizing the returned object.
            [void](ConvertTo-RifeArchitectureKeyFromCapability -ComputeCapability $capability)
            [pscustomobject]@{
                Name = $name.Trim()
                ComputeCapability = $capability.Trim()
            }
        }
    )
}

function Get-RifeArchitectureKeysForInventory([object[]]$GpuInventory) {
    $keys = @()
    foreach ($gpu in @($GpuInventory)) {
        $property = $gpu.PSObject.Properties['ComputeCapability']
        if (-not $property) {
            $property = $gpu.PSObject.Properties['computeCapability']
        }
        if (-not $property) {
            throw 'GPU inventory entry is missing computeCapability.'
        }

        $key = ConvertTo-RifeArchitectureKeyFromCapability -ComputeCapability ([string]$property.Value)
        if ($keys -notcontains $key) {
            $keys += $key
        }
    }
    $keys
}

function Read-RifeChecksumList([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Checksum file was not found: $Path"
    }

    $checksums = @{}
    foreach ($line in @(Get-Content -LiteralPath $Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -notmatch '^([0-9A-Fa-f]{64})\s+\*?(.+)$') {
            throw "Invalid checksum line in '$Path': $line"
        }

        $name = $Matches[2].Trim().Replace('\\', '/')
        if ($checksums.ContainsKey($name)) {
            throw "Checksum file '$Path' contains duplicate entry '$name'."
        }
        $checksums[$name] = $Matches[1].ToLowerInvariant()
    }
    $checksums
}

function Assert-RifeFileHash([string]$Path, [string]$ExpectedHash, [string]$DisplayName) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file was not found: $DisplayName"
    }
    if ([string]::IsNullOrWhiteSpace($ExpectedHash) -or $ExpectedHash -notmatch '^[0-9A-Fa-f]{64}$') {
        throw "Expected SHA-256 is invalid for $DisplayName"
    }

    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    $expected = $ExpectedHash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "SHA-256 mismatch for '$DisplayName': expected $expected, got $actual"
    }
    $actual
}

function Get-RifeTensorRtMajorMinor([string]$Version) {
    $match = [regex]::Match([string]$Version, '^(\d+)\.(\d+)(?:\.|$)')
    if (-not $match.Success) {
        throw "TensorRT version is invalid: $Version"
    }
    '{0}.{1}' -f $match.Groups[1].Value, $match.Groups[2].Value
}

Export-ModuleMember -Function @(
    'ConvertTo-RifeArchitectureKey',
    'ConvertTo-RifeArchitectureKeyFromCapability',
    'Get-RifeBuilderResourceName',
    'Get-RifeExpectedArchitectures',
    'Get-RifeCommonRuntimeFileNames',
    'Get-RifeRuntimeArchiveNames',
    'Get-RifeGpuInventory',
    'Get-RifeArchitectureKeysForInventory',
    'Read-RifeChecksumList',
    'Assert-RifeFileHash',
    'Get-RifeTensorRtMajorMinor'
)
