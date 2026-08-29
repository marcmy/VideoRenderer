$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'Source'

function Read-Normalized([string]$Path) {
    return (Get-Content -LiteralPath $Path -Raw).Replace("`r`n", "`n")
}

function Write-Utf8([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Replace-Exact([string]$Text, [string]$Needle, [string]$Replacement, [string]$Label) {
    $Needle = $Needle.Replace("`r`n", "`n")
    $Replacement = $Replacement.Replace("`r`n", "`n")
    $count = [regex]::Matches($Text, [regex]::Escape($Needle)).Count
    if ($count -ne 1) {
        throw "Expected exactly one $Label, found $count."
    }
    return $Text.Replace($Needle, $Replacement)
}

$seedPath = Join-Path $source 'NvidiaOpticalFlowDenseSeed.hlsl'
$repairPath = Join-Path $source 'NvidiaOpticalFlowDenseRepair.hlsl'
$headerPath = Join-Path $source 'NvidiaOpticalFlowDenseSynthesizer.h'
$cppPath = Join-Path $source 'NvidiaOpticalFlowDenseSynthesizer.cpp'

$seed = Read-Normalized $seedPath
if ($seed.Contains('RWTexture2D<uint2> CoverageAccum : register(u4);')) {
    $seed = Replace-Exact $seed `
        'RWTexture2D<uint2> CoverageAccum : register(u4);' `
        "RWTexture2D<uint> CoveragePrevious : register(u4);`nRWTexture2D<uint> CoverageNext : register(u5);" `
        'seed scalar coverage declarations'
    $seed = Replace-Exact $seed `
        'InterlockedAdd(CoverageAccum[cell].x, weight);' `
        'InterlockedAdd(CoveragePrevious[cell], weight);' `
        'previous coverage atomic'
    $seed = Replace-Exact $seed `
        'InterlockedAdd(CoverageAccum[cell].y, weight);' `
        'InterlockedAdd(CoverageNext[cell], weight);' `
        'next coverage atomic'
}
Write-Utf8 $seedPath $seed

$repair = Read-Normalized $repairPath
if ($repair.Contains('Texture2D<uint2> CoverageAccum : register(t1);')) {
    $repair = Replace-Exact $repair `
        'Texture2D<uint2> CoverageAccum : register(t1);' `
        "Texture2D<uint> CoveragePrevious : register(t1);`nTexture2D<uint> CoverageNext : register(t2);" `
        'repair scalar coverage declarations'
    $repair = Replace-Exact $repair `
        'return float2(CoverageAccum.Load(int3(cell, 0))) / CoverageAccumScale;' `
        "return float2(`n        CoveragePrevious.Load(int3(cell, 0)),`n        CoverageNext.Load(int3(cell, 0))) / CoverageAccumScale;" `
        'repair scalar coverage load'
}
Write-Utf8 $repairPath $repair

$header = Read-Normalized $headerPath
if (-not $header.Contains('m_coverageNextTexture')) {
    $header = Replace-Exact $header @'
    CComPtr<ID3D11Texture2D> m_coverageAccumTexture;
    CComPtr<ID3D11ShaderResourceView> m_coverageAccumView;
    CComPtr<ID3D11UnorderedAccessView> m_coverageAccumUav;
    CComPtr<ID3D11Texture2D> m_ownershipTexture;
'@ @'
    CComPtr<ID3D11Texture2D> m_coverageAccumTexture;
    CComPtr<ID3D11ShaderResourceView> m_coverageAccumView;
    CComPtr<ID3D11UnorderedAccessView> m_coverageAccumUav;
    CComPtr<ID3D11Texture2D> m_coverageNextTexture;
    CComPtr<ID3D11ShaderResourceView> m_coverageNextView;
    CComPtr<ID3D11UnorderedAccessView> m_coverageNextUav;
    CComPtr<ID3D11Texture2D> m_ownershipTexture;
'@ 'next coverage resource members'
}
Write-Utf8 $headerPath $header

$cpp = Read-Normalized $cppPath
if (-not $cpp.Contains('dense NVOF next-frame coverage accumulation')) {
    $cpp = Replace-Exact $cpp `
        'const std::array<ID3D11UnorderedAccessView*, 5> nullUavs = {};' `
        'const std::array<ID3D11UnorderedAccessView*, 6> nullUavs = {};' `
        'compute unbind UAV count'

    $cpp = Replace-Exact $cpp `
        'coverageDesc.Format = DXGI_FORMAT_R32G32_UINT;' `
        'coverageDesc.Format = DXGI_FORMAT_R32_UINT;' `
        'scalar coverage texture format'

    $nextCreation = @'
    hr = device->CreateTexture2D(&coverageDesc, nullptr, &m_coverageNextTexture);
    if (FAILED(hr)) {
        status = std::format(L"CreateTexture2D(dense NVOF next-frame coverage accumulation) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateShaderResourceView(m_coverageNextTexture, nullptr, &m_coverageNextView);
    if (FAILED(hr)) {
        status = std::format(L"CreateShaderResourceView(dense NVOF next-frame coverage accumulation) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateUnorderedAccessView(m_coverageNextTexture, nullptr, &m_coverageNextUav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView(dense NVOF next-frame coverage accumulation) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

'@
    $coverageUavBlock = @'
    hr = device->CreateUnorderedAccessView(m_coverageAccumTexture, nullptr, &m_coverageAccumUav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView(dense NVOF coverage accumulation) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

'@
    $cpp = Replace-Exact $cpp $coverageUavBlock ($coverageUavBlock + $nextCreation) 'next coverage texture creation point'

    $cpp = Replace-Exact $cpp @'
    m_coverageAccumUav.Release();
    m_coverageAccumView.Release();
    m_coverageAccumTexture.Release();
    m_unsafeCellUav.Release();
'@ @'
    m_coverageNextUav.Release();
    m_coverageNextView.Release();
    m_coverageNextTexture.Release();
    m_coverageAccumUav.Release();
    m_coverageAccumView.Release();
    m_coverageAccumTexture.Release();
    m_unsafeCellUav.Release();
'@ 'next coverage reset resources'

    $cpp = Replace-Exact $cpp @'
            !m_coverageAccumUav || !m_coverageAccumView || !m_ownershipUav || !m_ownershipView ||
'@ @'
            !m_coverageAccumUav || !m_coverageAccumView || !m_coverageNextUav || !m_coverageNextView ||
            !m_ownershipUav || !m_ownershipView ||
'@ 'next coverage resource validation'

    $cpp = Replace-Exact $cpp @'
    context->ClearUnorderedAccessViewUint(m_coverageAccumUav, zero);
'@ @'
    context->ClearUnorderedAccessViewUint(m_coverageAccumUav, zero);
    context->ClearUnorderedAccessViewUint(m_coverageNextUav, zero);
'@ 'next coverage accumulator clear'

    $cpp = Replace-Exact $cpp @'
    const std::array<ID3D11UnorderedAccessView*, 5> seedOutputs = {
        m_seedUavs[0], m_qualityUav, m_unsafeCellUav, m_repairUavs[0], m_coverageAccumUav,
    };
'@ @'
    const std::array<ID3D11UnorderedAccessView*, 6> seedOutputs = {
        m_seedUavs[0], m_qualityUav, m_unsafeCellUav, m_repairUavs[0],
        m_coverageAccumUav, m_coverageNextUav,
    };
'@ 'seed scalar coverage bindings'

    $cpp = Replace-Exact $cpp @'
    const std::array<ID3D11ShaderResourceView*, 2> repairInputs = {
        m_repairViews[0], m_coverageAccumView,
    };
'@ @'
    const std::array<ID3D11ShaderResourceView*, 3> repairInputs = {
        m_repairViews[0], m_coverageAccumView, m_coverageNextView,
    };
'@ 'repair scalar coverage bindings'
}
Write-Utf8 $cppPath $cpp

Write-Host 'Converted COV-FB55 coverage accumulation to two SM5-compatible scalar UAVs.'
