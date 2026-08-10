$ErrorActionPreference = 'Stop'

function Read-Lf([string]$Path) {
    return ([IO.File]::ReadAllText($Path) -replace "`r`n", "`n")
}
function Write-Lf([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text)
}
function Replace-One([string]$Text, [string]$Old, [string]$New, [string]$Label) {
    $first = $Text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Patch target not found: $Label" }
    $second = $Text.IndexOf($Old, $first + $Old.Length, [StringComparison]::Ordinal)
    if ($second -ge 0) { throw "Patch target is not unique: $Label" }
    return $Text.Substring(0, $first) + $New + $Text.Substring($first + $Old.Length)
}

# 1) Regional HLSL becomes a measurement of the worst local cluster.
$path = 'Source/NvidiaOpticalFlowDenseRegionGate.hlsl'
$text = Read-Lf $path
$text = Replace-One $text @'
    // Do not splice real-frame patches into the synthetic image. If a genuine
    // local cluster of catastrophic NVOF cells exists, reject the entire
    // inserted midpoint so spatial coherence is preserved.
'@ @'
    // Observe only: retain the largest local catastrophic cluster. Ordinary
    // 24p motion blur can also produce dense unsafe neighborhoods, so this
    // occupancy metric must not directly control presentation.
'@ 'regional diagnostic comment'
$text = Replace-One $text @'
    if (unsafeCount >= MinUnsafeCells) {
        InterlockedOr(RegionReject[uint2(0, 0)], 1u);
    }
'@ @'
    InterlockedMax(RegionReject[uint2(0, 0)], unsafeCount);
'@ 'regional max output'
Write-Lf $path $text

# 2) Warp presentation ignores the regional metric; only global 25% remains.
$path = 'Source/NvidiaOpticalFlowDenseWarp.hlsl'
$text = Read-Lf $path
$text = Replace-One $text 'Texture2D<uint> RegionReject : register(t4);' '' 'warp regional texture'
$text = Replace-One $text '    bool rejectRegion = RegionReject.Load(int3(0, 0, 0)) != 0u;' '' 'warp regional load'
$text = Replace-One $text '    if (unsafeFraction >= RepeatBadFraction || rejectRegion) {' '    if (unsafeFraction >= RepeatBadFraction) {' 'warp rejection condition'
Write-Lf $path $text

# 3) Dense synthesizer header: telemetry API and three-frame staging ring.
$path = 'Source/NvidiaOpticalFlowDenseSynthesizer.h'
$text = Read-Lf $path
$text = Replace-One $text '    void Reset();' "    void Reset();`n    std::wstring GetTelemetryText() const;" 'telemetry getter declaration'
$anchor = '    CComPtr<ID3D11UnorderedAccessView> m_regionRejectUav;'
$insert = @'
    CComPtr<ID3D11UnorderedAccessView> m_regionRejectUav;

    static constexpr UINT TelemetrySlotCount = 3;
    CComPtr<ID3D11Texture2D> m_qualityReadback[TelemetrySlotCount];
    CComPtr<ID3D11Texture2D> m_regionReadback[TelemetrySlotCount];
    bool m_telemetryPrimed[TelemetrySlotCount] = {};
    UINT m_telemetryWriteIndex = 0;
    UINT m_lastUnsafeCount = 0;
    UINT m_lastMaxLocalUnsafe = 0;
    bool m_haveTelemetry = false;
'@
$text = Replace-One $text $anchor $insert 'telemetry members'
Write-Lf $path $text

# 4) Dense synthesizer implementation.
$path = 'Source/NvidiaOpticalFlowDenseSynthesizer.cpp'
$text = Read-Lf $path

# Create staging textures immediately before full-resolution dense flow.
$anchor = '    D3D11_TEXTURE2D_DESC denseDesc = {};'
$insert = @'
    D3D11_TEXTURE2D_DESC telemetryDesc = qualityDesc;
    telemetryDesc.Usage = D3D11_USAGE_STAGING;
    telemetryDesc.BindFlags = 0;
    telemetryDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (UINT slot = 0; slot < TelemetrySlotCount; ++slot) {
        hr = device->CreateTexture2D(&telemetryDesc, nullptr, &m_qualityReadback[slot]);
        if (FAILED(hr)) {
            status = std::format(L"CreateTexture2D(dense NVOF quality telemetry {}) failed ({})", slot, HR2Str(hr));
            Reset();
            return false;
        }
        hr = device->CreateTexture2D(&telemetryDesc, nullptr, &m_regionReadback[slot]);
        if (FAILED(hr)) {
            status = std::format(L"CreateTexture2D(dense NVOF regional telemetry {}) failed ({})", slot, HR2Str(hr));
            Reset();
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC denseDesc = {};
'@
$text = Replace-One $text $anchor $insert 'staging telemetry creation'

# Release/reset telemetry state.
$anchor = '    m_unsafeCellUav.Release();'
$insert = @'
    for (UINT slot = 0; slot < TelemetrySlotCount; ++slot) {
        m_qualityReadback[slot].Release();
        m_regionReadback[slot].Release();
        m_telemetryPrimed[slot] = false;
    }
    m_telemetryWriteIndex = 0;
    m_lastUnsafeCount = 0;
    m_lastMaxLocalUnsafe = 0;
    m_haveTelemetry = false;
    m_unsafeCellUav.Release();
'@
$text = Replace-One $text $anchor $insert 'telemetry reset'

# Regional pass is observe-only; MinUnsafeCells remains ABI padding for now.
$text = Replace-One $text '        m_flowWidth, m_flowHeight, 18u, 3u,' '        m_flowWidth, m_flowHeight, 0u, 3u,' 'observe-only region parameters'
$text = Replace-One $text @'
    // A local catastrophic cluster can be visually unacceptable even when it
    // occupies far less than the global 25% threshold. Reject the entire
    // inserted midpoint rather than compositing real-frame patches locally.
'@ @'
    // Observe local catastrophic occupancy without allowing it to reject a
    // midpoint. The previous 8/49 and 18/49 rules both collapsed ordinary
    // 23.976p film motion back toward 24p.
'@ 'observe-only region comment'

# Queue/read the two 1x1 metrics after the regional compute pass.
$anchor = "    UnbindCompute(context);`n`n    UINT jumpStep = 1;"
$insert = @'
    UnbindCompute(context);

    // Read a slot written three midpoint submissions ago. DO_NOT_WAIT means
    // diagnostics can never stall video presentation. Then queue this frame's
    // counters into the same staging slot for a future read.
    const UINT telemetrySlot = m_telemetryWriteIndex;
    if (m_telemetryPrimed[telemetrySlot]) {
        D3D11_MAPPED_SUBRESOURCE qualityMapped = {};
        const HRESULT qualityHr = context->Map(
            m_qualityReadback[telemetrySlot], 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &qualityMapped);
        if (SUCCEEDED(qualityHr)) {
            D3D11_MAPPED_SUBRESOURCE regionMapped = {};
            const HRESULT regionHr = context->Map(
                m_regionReadback[telemetrySlot], 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &regionMapped);
            if (SUCCEEDED(regionHr)) {
                m_lastUnsafeCount = *static_cast<const UINT*>(qualityMapped.pData);
                m_lastMaxLocalUnsafe = *static_cast<const UINT*>(regionMapped.pData);
                m_haveTelemetry = true;
                context->Unmap(m_regionReadback[telemetrySlot], 0);
            }
            context->Unmap(m_qualityReadback[telemetrySlot], 0);
        }
    }
    context->CopyResource(m_qualityReadback[telemetrySlot], m_qualityTexture);
    context->CopyResource(m_regionReadback[telemetrySlot], m_regionRejectTexture);
    m_telemetryPrimed[telemetrySlot] = true;
    m_telemetryWriteIndex = (telemetrySlot + 1) % TelemetrySlotCount;

    UINT jumpStep = 1;
'@
$text = Replace-One $text $anchor $insert 'async telemetry queue/read'

# Warp no longer binds the regional diagnostic texture.
$old = @'
    const std::array<ID3D11ShaderResourceView*, 5> warpInputs = {
        previousFrame, nextFrame, m_denseFlowView, m_qualityView, m_regionRejectView,
    };
'@
$new = @'
    const std::array<ID3D11ShaderResourceView*, 4> warpInputs = {
        previousFrame, nextFrame, m_denseFlowView, m_qualityView,
    };
'@
$text = Replace-One $text $old $new 'warp input SRVs'

# Insert telemetry formatter before Dispatch.
$marker = 'bool CNvidiaOpticalFlowDenseSynthesizer::Dispatch(ID3D11DeviceContext* context,'
$getter = @'
std::wstring CNvidiaOpticalFlowDenseSynthesizer::GetTelemetryText() const
{
    if (!m_haveTelemetry || !m_flowWidth || !m_flowHeight) {
        return L"quality telemetry warming up";
    }
    const UINT cellCount = m_flowWidth * m_flowHeight;
    const double badPercent = 100.0 * static_cast<double>(m_lastUnsafeCount) /
        std::max(1u, cellCount);
    return std::format(
        L"bad {:.1f}% ({}/{}), worst7x7 {}/49, would8={}, would18={}",
        badPercent, m_lastUnsafeCount, cellCount, m_lastMaxLocalUnsafe,
        m_lastMaxLocalUnsafe >= 8 ? L"yes" : L"no",
        m_lastMaxLocalUnsafe >= 18 ? L"yes" : L"no");
}

bool CNvidiaOpticalFlowDenseSynthesizer::Dispatch(ID3D11DeviceContext* context,
'@
$text = Replace-One $text $marker $getter 'telemetry formatter'
Write-Lf $path $text

# 5) Surface telemetry in the existing MPCVR status text.
$path = 'Source/NvidiaOpticalFlowNative.cpp'
$text = Read-Lf $path
$text = Replace-One $text 'validated jump-flood dense flow + edge-aware next-frame warp + frame quality gate; live cost disabled' 'validated jump-flood dense flow + edge-aware next-frame warp + global frame quality gate; regional diagnostics observe-only; live cost disabled' 'runtime info'
$old = @'
		status = std::format(
			L"Native NVOF active ({:.2f} ms submit, t={:.3f})",
			processTimeMs, midpointTime);
'@
$new = @'
		const std::wstring telemetry = denseSynthesizer
			? denseSynthesizer->GetTelemetryText()
			: L"quality telemetry unavailable";
		status = std::format(
			L"Native NVOF active ({:.2f} ms submit, t={:.3f}); {}",
			processTimeMs, midpointTime, telemetry);
'@
$text = Replace-One $text $old $new 'native status telemetry'
Write-Lf $path $text

# 6) Recompile changed SM5 shaders into committed bytecode.
$sdkBin = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$fxc = Get-ChildItem $sdkBin -Filter fxc.exe -Recurse -File |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $fxc) { throw 'fxc.exe was not found in the Windows SDK.' }
$specs = @(
    @{ Source='Source/NvidiaOpticalFlowDenseRegionGate.hlsl'; Header='Source/NvidiaOpticalFlowDenseRegionGateBytecode.h'; Name='g_NvofDenseRegionGateBytecode' },
    @{ Source='Source/NvidiaOpticalFlowDenseWarp.hlsl'; Header='Source/NvidiaOpticalFlowDenseWarpBytecode.h'; Name='g_NvofDenseWarpBytecode' }
)
foreach ($spec in $specs) {
    & $fxc.FullName /nologo /T cs_5_0 /E main /O3 /WX /Fh $spec.Header /Vn $spec.Name $spec.Source
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add -- Source/NvidiaOpticalFlowDenseRegionGate.hlsl Source/NvidiaOpticalFlowDenseWarp.hlsl Source/NvidiaOpticalFlowDenseRegionGateBytecode.h Source/NvidiaOpticalFlowDenseWarpBytecode.h Source/NvidiaOpticalFlowDenseSynthesizer.h Source/NvidiaOpticalFlowDenseSynthesizer.cpp Source/NvidiaOpticalFlowNative.cpp
git commit -m 'Make regional NVOF detector observe-only with telemetry'
git push origin HEAD:feature/native-nvof-interpolation
