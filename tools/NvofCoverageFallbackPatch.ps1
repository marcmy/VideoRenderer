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
    $count = [regex]::Matches($Text, [regex]::Escape($Needle)).Count
    if ($count -ne 1) {
        throw "Expected exactly one $Label, found $count."
    }
    return $Text.Replace($Needle, $Replacement)
}

$seedPath = Join-Path $source 'NvidiaOpticalFlowDenseSeed.hlsl'
$repairPath = Join-Path $source 'NvidiaOpticalFlowDenseRepair.hlsl'
$warpPath = Join-Path $source 'NvidiaOpticalFlowDenseWarp.hlsl'
$headerPath = Join-Path $source 'NvidiaOpticalFlowDenseSynthesizer.h'
$cppPath = Join-Path $source 'NvidiaOpticalFlowDenseSynthesizer.cpp'

foreach ($path in @($seedPath, $repairPath, $warpPath, $headerPath, $cppPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing experiment source: $path"
    }
}

# -----------------------------------------------------------------------------
# Seed pass: accumulate directional A/B coverage at the actual interpolation
# phase. Coverage is bilinearly splatted on the native 4x4 NVOF grid. This is
# ownership evidence only; it never makes an invalid motion vector trustworthy.
# -----------------------------------------------------------------------------
$seed = Read-Normalized $seedPath
if (-not $seed.Contains('RWTexture2D<uint2> CoverageAccum : register(u4);')) {
    $seed = Replace-Exact $seed `
        'RWTexture2D<float4> RepairCandidate : register(u3);' `
        "RWTexture2D<float4> RepairCandidate : register(u3);`nRWTexture2D<uint2> CoverageAccum : register(u4);" `
        'seed coverage UAV declaration'

    $seed = Replace-Exact $seed @'
    float CutCorrelationThreshold;
    float CutMadThreshold;
    float2 Padding;
'@ @'
    float CutCorrelationThreshold;
    float CutMadThreshold;
    float MidpointTime;
    float Padding;
'@ 'seed midpoint parameter'

    $coverageHelpers = @'
static const float CoverageAccumScale = 1024.0;

void AddCoverageSample(int2 cell, uint weight, bool previousSide)
{
    if (weight == 0u || any(cell < 0) || any(cell >= int2(FlowSize))) return;
    if (previousSide) {
        InterlockedAdd(CoverageAccum[cell].x, weight);
    }
    else {
        InterlockedAdd(CoverageAccum[cell].y, weight);
    }
}

void SplatCoverage(float2 projectedGrid, bool previousSide)
{
    int2 base = int2(floor(projectedGrid));
    float2 f = frac(projectedGrid);
    float4 weights = float4(
        (1.0 - f.x) * (1.0 - f.y),
        f.x * (1.0 - f.y),
        (1.0 - f.x) * f.y,
        f.x * f.y);
    uint4 quantized = (uint4)round(saturate(weights) * CoverageAccumScale);

    AddCoverageSample(base, quantized.x, previousSide);
    AddCoverageSample(base + int2(1, 0), quantized.y, previousSide);
    AddCoverageSample(base + int2(0, 1), quantized.z, previousSide);
    AddCoverageSample(base + int2(1, 1), quantized.w, previousSide);
}

'@
    $seed = Replace-Exact $seed '[numthreads(8, 8, 1)]' ($coverageHelpers + '[numthreads(8, 8, 1)]') 'seed shader entry marker'

    $seed = Replace-Exact $seed @'
    float2 bToA = LoadFlow(ForwardFlowBtoA, cell);
    float2 aToB = LoadFlow(BackwardFlowAtoB, cell);

    float bToAError = length(bToA + SampleFlow(BackwardFlowAtoB, pixel + bToA));
'@ @'
    float2 bToA = LoadFlow(ForwardFlowBtoA, cell);
    float2 aToB = LoadFlow(BackwardFlowAtoB, cell);

    // Directional visibility/ownership evidence. A->B vectors originate on the
    // previous frame and move forward by t; B->A vectors originate on the next
    // frame and move backward by (1-t). Keep this independent from consistency.
    SplatCoverage(float2(cell) + MidpointTime * (aToB / GridSize), true);
    SplatCoverage(float2(cell) + (1.0 - MidpointTime) * (bToA / GridSize), false);

    float bToAError = length(bToA + SampleFlow(BackwardFlowAtoB, pixel + bToA));
'@ 'seed coverage splat call'
}
Write-Utf8 $seedPath $seed

# -----------------------------------------------------------------------------
# Repair pass: turn atomic coverage splats into the tolerant SVP-style local
# 3x3 ownership cue used by the offline corpus replay. /8 normalization and the
# 0.15 denominator floor intentionally match the validated clean-room prototype.
# -----------------------------------------------------------------------------
$repair = Read-Normalized $repairPath
if (-not $repair.Contains('RWTexture2D<float> OwnershipField : register(u1);')) {
    $repair = Replace-Exact $repair @'
Texture2D<float4> RepairCandidate : register(t0);
RWTexture2D<float4> RepairField : register(u0);
'@ @'
Texture2D<float4> RepairCandidate : register(t0);
Texture2D<uint2> CoverageAccum : register(t1);
RWTexture2D<float4> RepairField : register(u0);
RWTexture2D<float> OwnershipField : register(u1);
'@ 'repair coverage resources'

    $repairHelpers = @'
static const float CoverageAccumScale = 1024.0;

float2 LoadCoverage(int2 cell)
{
    if (any(cell < 0) || any(cell >= int2(FlowSize))) return 0.0;
    return float2(CoverageAccum.Load(int3(cell, 0))) / CoverageAccumScale;
}

float2 LocalCoverage(int2 center)
{
    float2 total = 0.0;
    [unroll]
    for (int oy = -1; oy <= 1; ++oy) {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox) {
            total += LoadCoverage(center + int2(ox, oy));
        }
    }
    return min(total / 8.0, float2(1.25, 1.25));
}

'@
    $repair = Replace-Exact $repair '[numthreads(8, 8, 1)]' ($repairHelpers + '[numthreads(8, 8, 1)]') 'repair shader entry marker'

    $repair = Replace-Exact $repair @'
    RepairField[id.xy] = float4(
        repairMotion,
        saturate(repairMask),
        saturate(unsupportedMask));
'@ @'
    float2 coverage = LocalCoverage(center);
    float ownershipDenom = max(coverage.x + coverage.y, 0.15);
    float ownership = clamp((coverage.x - coverage.y) / ownershipDenom, -1.0, 1.0);
    OwnershipField[id.xy] = ownership;

    RepairField[id.xy] = float4(
        repairMotion,
        saturate(repairMask),
        saturate(unsupportedMask));
'@ 'repair ownership output'
}
Write-Utf8 $repairPath $repair

# -----------------------------------------------------------------------------
# Warp pass: leave all successful golden interpolation untouched. Only when the
# golden result is already effectively temporal fallback, and only in the core
# unsupported mask, apply at most a 55/45 A/B bias selected by ownership.
# -----------------------------------------------------------------------------
$warp = Read-Normalized $warpPath
if (-not $warp.Contains('Texture2D<float> OwnershipField : register(t5);')) {
    $warp = Replace-Exact $warp `
        'Texture2D<float4> RepairField : register(t4);' `
        "Texture2D<float4> RepairField : register(t4);`nTexture2D<float> OwnershipField : register(t5);" `
        'warp ownership texture declaration'

    $ownershipHelper = @'
float SampleOwnership(float2 pixel)
{
    uint ownershipWidth, ownershipHeight;
    OwnershipField.GetDimensions(ownershipWidth, ownershipHeight);
    float2 ownershipGrid = pixel / max(RepairGridSize, 1.0e-6);
    float2 ownershipUv = (ownershipGrid + 0.5) / float2(ownershipWidth, ownershipHeight);
    return OwnershipField.SampleLevel(LinearClamp, ownershipUv, 0.0);
}

'@
    $warp = Replace-Exact $warp 'float MappingTopologyReject(float2 columnX, float2 columnY)' ($ownershipHelper + 'float MappingTopologyReject(float2 columnX, float2 columnY)') 'warp ownership helper insertion point'

    $warp = Replace-Exact $warp @'
    if (localFallback > 1.0e-4) {
        float4 temporalMidpoint = lerp(
            SampleFrame(PreviousFrame, target),
            SampleFrame(NextFrame, target),
            MidpointTime);
        current = lerp(current, temporalMidpoint, localFallback);
    }
'@ @'
    if (localFallback > 1.0e-4) {
        float4 fallbackPrevious = SampleFrame(PreviousFrame, target);
        float4 fallbackNext = SampleFrame(NextFrame, target);
        float4 temporalMidpoint = lerp(fallbackPrevious, fallbackNext, MidpointTime);

        // First compute the exact golden-baseline result. Ownership is allowed to
        // touch only pixels that this result has already pushed to within roughly
        // 1..4 mean RGB LSBs of unwarped temporal fallback.
        float4 goldenCurrent = lerp(current, temporalMidpoint, localFallback);
        float fallbackDistance = dot(
            abs(goldenCurrent.rgb - temporalMidpoint.rgb),
            float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
        float fallbackConfidence = 1.0 - smoothstep(1.0 / 255.0, 4.0 / 255.0, fallbackDistance);

        float ownership = SampleOwnership(target);
        float ownershipConfidence = smoothstep(0.15, 0.45, abs(ownership));
        // RepairField.W is deliberately much wider than the original unsupported
        // core. Restrict the experiment to its near-1 center so successful dense
        // interpolation outside the quarantine cannot be recolored by coverage.
        float unsupportedCore = smoothstep(0.90, 1.0, unsupportedMask);
        float phaseScale = saturate(4.0 * MidpointTime * (1.0 - MidpointTime));
        float ownershipDelta = 0.05 * phaseScale * localFallback * unsupportedCore
            * ownershipConfidence * fallbackConfidence * sign(ownership);

        current = goldenCurrent;
        current.rgb = saturate(
            goldenCurrent.rgb + ownershipDelta * (fallbackPrevious.rgb - fallbackNext.rgb));
    }
'@ 'warp temporal fallback block'
}
Write-Utf8 $warpPath $warp

# -----------------------------------------------------------------------------
# C++ resource plumbing. Use two small coarse-grid textures: uint2 atomic
# coverage accumulation and linearly filtered R32_FLOAT ownership. No readback,
# no CPU stall, and no additional dispatch: Repair resolves coverage in-place.
# -----------------------------------------------------------------------------
$header = Read-Normalized $headerPath
if (-not $header.Contains('m_coverageAccumTexture')) {
    $header = Replace-Exact $header @'
        float cutCorrelationThreshold;
        float cutMadThreshold;
        float padding[2];
'@ @'
        float cutCorrelationThreshold;
        float cutMadThreshold;
        float midpointTime;
        float padding;
'@ 'SeedParameters midpoint fields'

    $header = Replace-Exact $header @'
    CComPtr<ID3D11Texture2D> m_unsafeCellTexture;
    CComPtr<ID3D11ShaderResourceView> m_unsafeCellView;
    CComPtr<ID3D11UnorderedAccessView> m_unsafeCellUav;

    CComPtr<ID3D11Texture2D> m_regionRejectTexture;
'@ @'
    CComPtr<ID3D11Texture2D> m_unsafeCellTexture;
    CComPtr<ID3D11ShaderResourceView> m_unsafeCellView;
    CComPtr<ID3D11UnorderedAccessView> m_unsafeCellUav;

    CComPtr<ID3D11Texture2D> m_coverageAccumTexture;
    CComPtr<ID3D11ShaderResourceView> m_coverageAccumView;
    CComPtr<ID3D11UnorderedAccessView> m_coverageAccumUav;
    CComPtr<ID3D11Texture2D> m_ownershipTexture;
    CComPtr<ID3D11ShaderResourceView> m_ownershipView;
    CComPtr<ID3D11UnorderedAccessView> m_ownershipUav;

    CComPtr<ID3D11Texture2D> m_regionRejectTexture;
'@ 'coverage resource members'
}
Write-Utf8 $headerPath $header

$cpp = Read-Normalized $cppPath
if (-not $cpp.Contains('dense NVOF coverage accumulation')) {
    $cpp = Replace-Exact $cpp @'
    const std::array<ID3D11ShaderResourceView*, 5> nullSrvs = {};
    const std::array<ID3D11UnorderedAccessView*, 4> nullUavs = {};
'@ @'
    const std::array<ID3D11ShaderResourceView*, 6> nullSrvs = {};
    const std::array<ID3D11UnorderedAccessView*, 5> nullUavs = {};
'@ 'compute unbind resource counts'

    $coverageCreation = @'
    D3D11_TEXTURE2D_DESC coverageDesc = seedDesc;
    coverageDesc.Format = DXGI_FORMAT_R32G32_UINT;
    hr = device->CreateTexture2D(&coverageDesc, nullptr, &m_coverageAccumTexture);
    if (FAILED(hr)) {
        status = std::format(L"CreateTexture2D(dense NVOF coverage accumulation) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateShaderResourceView(m_coverageAccumTexture, nullptr, &m_coverageAccumView);
    if (FAILED(hr)) {
        status = std::format(L"CreateShaderResourceView(dense NVOF coverage accumulation) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateUnorderedAccessView(m_coverageAccumTexture, nullptr, &m_coverageAccumUav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView(dense NVOF coverage accumulation) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

    D3D11_TEXTURE2D_DESC ownershipDesc = seedDesc;
    ownershipDesc.Format = DXGI_FORMAT_R32_FLOAT;
    hr = device->CreateTexture2D(&ownershipDesc, nullptr, &m_ownershipTexture);
    if (FAILED(hr)) {
        status = std::format(L"CreateTexture2D(dense NVOF ownership field) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateShaderResourceView(m_ownershipTexture, nullptr, &m_ownershipView);
    if (FAILED(hr)) {
        status = std::format(L"CreateShaderResourceView(dense NVOF ownership field) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }
    hr = device->CreateUnorderedAccessView(m_ownershipTexture, nullptr, &m_ownershipUav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView(dense NVOF ownership field) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

'@
    $cpp = Replace-Exact $cpp `
        '    hr = device->CreateTexture2D(&qualityDesc, nullptr, &m_regionRejectTexture);' `
        ($coverageCreation + '    hr = device->CreateTexture2D(&qualityDesc, nullptr, &m_regionRejectTexture);') `
        'coverage texture insertion point'

    $cpp = Replace-Exact $cpp @'
    m_haveTelemetry = false;
    m_unsafeCellUav.Release();
'@ @'
    m_haveTelemetry = false;
    m_ownershipUav.Release();
    m_ownershipView.Release();
    m_ownershipTexture.Release();
    m_coverageAccumUav.Release();
    m_coverageAccumView.Release();
    m_coverageAccumTexture.Release();
    m_unsafeCellUav.Release();
'@ 'coverage reset resources'

    $cpp = Replace-Exact $cpp `
        'guard=local-photo, seeds=asym-backfill' `
        'guard=local-photo+covfb55, seeds=asym-backfill' `
        'telemetry experiment label'

    $cpp = Replace-Exact $cpp @'
            !m_qualityUav || !m_qualityView || !m_unsafeCellUav || !m_unsafeCellView ||
            !m_repairUavs[0] || !m_repairViews[0] || !m_repairUavs[1] || !m_repairViews[1] ||
'@ @'
            !m_qualityUav || !m_qualityView || !m_unsafeCellUav || !m_unsafeCellView ||
            !m_coverageAccumUav || !m_coverageAccumView || !m_ownershipUav || !m_ownershipView ||
            !m_repairUavs[0] || !m_repairViews[0] || !m_repairUavs[1] || !m_repairViews[1] ||
'@ 'dispatch coverage resource validation'

    $cpp = Replace-Exact $cpp @'
    context->ClearUnorderedAccessViewUint(m_qualityUav, zero);
    context->ClearUnorderedAccessViewUint(m_regionRejectUav, zero);
'@ @'
    context->ClearUnorderedAccessViewUint(m_qualityUav, zero);
    context->ClearUnorderedAccessViewUint(m_regionRejectUav, zero);
    context->ClearUnorderedAccessViewUint(m_coverageAccumUav, zero);
'@ 'coverage accumulator clear'

    $cpp = Replace-Exact $cpp @'
        m_flowWidth, m_flowHeight, 4.0f, 20.0f,
        20.0f, m_frameWidth, m_frameHeight, 0.86f,
        0.15f, 0.055f, {0.0f, 0.0f},
'@ @'
        m_flowWidth, m_flowHeight, 4.0f, 20.0f,
        20.0f, m_frameWidth, m_frameHeight, 0.86f,
        0.15f, 0.055f, midpointTime, 0.0f,
'@ 'seed midpoint initializer'

    $cpp = Replace-Exact $cpp @'
    const std::array<ID3D11UnorderedAccessView*, 4> seedOutputs = {
        m_seedUavs[0], m_qualityUav, m_unsafeCellUav, m_repairUavs[0],
    };
'@ @'
    const std::array<ID3D11UnorderedAccessView*, 5> seedOutputs = {
        m_seedUavs[0], m_qualityUav, m_unsafeCellUav, m_repairUavs[0], m_coverageAccumUav,
    };
'@ 'seed coverage output binding'

    $cpp = Replace-Exact $cpp @'
    ID3D11ShaderResourceView* repairInput = m_repairViews[0];
    ID3D11UnorderedAccessView* repairOutput = m_repairUavs[1];
    context->CSSetShader(m_repairShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &repairBuffer);
    context->CSSetShaderResources(0, 1, &repairInput);
    context->CSSetUnorderedAccessViews(0, 1, &repairOutput, nullptr);
'@ @'
    const std::array<ID3D11ShaderResourceView*, 2> repairInputs = {
        m_repairViews[0], m_coverageAccumView,
    };
    const std::array<ID3D11UnorderedAccessView*, 2> repairOutputs = {
        m_repairUavs[1], m_ownershipUav,
    };
    context->CSSetShader(m_repairShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &repairBuffer);
    context->CSSetShaderResources(0, static_cast<UINT>(repairInputs.size()), repairInputs.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(repairOutputs.size()), repairOutputs.data(), nullptr);
'@ 'repair coverage resolve bindings'

    $cpp = Replace-Exact $cpp @'
    const std::array<ID3D11ShaderResourceView*, 5> warpInputs = {
        previousFrame, nextFrame, m_denseFlowView, m_qualityView, m_repairViews[1],
    };
'@ @'
    const std::array<ID3D11ShaderResourceView*, 6> warpInputs = {
        previousFrame, nextFrame, m_denseFlowView, m_qualityView, m_repairViews[1], m_ownershipView,
    };
'@ 'warp ownership input binding'
}
Write-Utf8 $cppPath $cpp

Write-Host 'Applied NVOF COV-FB55 ownership-biased fallback experiment.'