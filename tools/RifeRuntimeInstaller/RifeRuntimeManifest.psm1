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

Export-ModuleMember -Function @(
    'ConvertTo-RifeArchitectureKey',
    'Get-RifeBuilderResourceName',
    'Get-RifeExpectedArchitectures',
    'Get-RifeCommonRuntimeFileNames',
    'Get-RifeRuntimeArchiveNames'
)
