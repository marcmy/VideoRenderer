$ErrorActionPreference = 'Stop'

function Read-Text([string]$Path) { [IO.File]::ReadAllText($Path) }
function Write-Text([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text) }
function Replace-Required([string]$Text, [string]$Old, [string]$New, [string]$Label) {
    if (-not $Text.Contains($Old)) { throw "Patch target not found: $Label" }
    return $Text.Replace($Old, $New)
}

# Regional detector: keep measuring the worst 7x7 unsafe-cell cluster, but never
# turn that measurement into a presentation decision.
$path = 'Source/NvidiaOpticalFlowDenseRegionGate.hlsl'
$text = Read-Text $path
$old = @'
    // Do not splice real-frame patches into the synthetic image. If a genuine
    // local cluster of catastrophic NVOF cells exists, reject the entire
    // inserted midpoint so spatial coherence is preserved.
    if (unsafeCount >= MinUnsafeCells) {
        InterlockedOr(RegionReject[uint2(0, 0)], 1u);
    }
'@
$new = @'
    // Observe only: retain the largest local catastrophic cluster for
    // asynchronous diagnostics. Presentation must not be rejected from this
    // occupancy metric because ordinary 24p motion blur can also score highly.
    InterlockedMax(RegionReject[uint2(0, 0)], unsafeCount);
'@
$text = Replace-Required $text $old $new 'regional gate output'
Write-Text $path $text

# Final warp: only the proven global catastrophic-cell fraction may repeat a
# midpoint. The regional metric is no longer bound or consulted by presentation.
$path = 'Source/NvidiaOpticalFlowDenseWarp.hlsl'
$text = Read-Text $path
$text = Replace-Required $text "Texture2D<uint> RegionReject : register(t4);`n" '' 'warp regional SRV declaration'
$text = Replace-Required $text "    bool rejectRegion = RegionReject.Load(int3(0, 0, 0)) != 0u;`n    if (unsafeFraction >= RepeatBadFraction || rejectRegion) {" "    if (unsafeFraction >= RepeatBadFraction) {" 'warp regional reject condition'
Write-Text $path $text

# Public synthesizer telemetry accessor + asynchronous readback resources.
$path = 'Source/NvidiaOpticalFlowDenseSynthesizer.h'
$text = Read-Text $path
$text = Replace-Required $text "    void Reset();`n`n    bool Dispatch" "    void Reset();`n    std::wstring GetTelemetryText() const;`n`n    bool Dispatch" 'telemetry getter declaration'
$old = @'
    CComPtr<ID3D11Texture2D> m_regionRejectTexture;
    CComPtr<ID3D11ShaderResourceView> m_regionRejectView;
    CComPtr<ID3D11UnorderedAccessView> m_regionRejectUav;

    CComPtr<ID3D11Texture2D> m_denseFlowTexture;
'@
$new = @'
    CComPtr<ID3D11Texture2D> m_regionRejectTexture;
    CComPtr<ID3D11ShaderResourceView> m_regionRejectView;
    CComPtr<ID3D11UnorderedAccessView> m_regionRejectUav;

    static constexpr UINT TelemetrySlotCount = 3;
    CComPtr<ID3D11Texture2D> m_qualityReadback[TelemetrySlotCount];
    CComPtr<ID3D11Texture2D> m_regionReadback[TelemetrySlotCount];
    bool m_telemetryPrimed[TelemetrySlotCount] = {};
    UINT m_telemetryWriteIndex = 0;
    UINT m_lastUnsafeCount = 0;
    UINT m_lastMaxLocalUnsafe = 0;
    bool m_haveTelemetry = false;

    CComPtr<ID3D11Texture2D> m_denseFlowTexture;
'@
$text = Replace-Required $text $old $new 'telemetry resource members'
Write-Text $path $text

# Synthesizer: create/read/recycle staging telemetry without blocking the render
# thread. Keep the regional pass as observe-only and remove it from warp inputs.
$path = 'Source/NvidiaOpticalFlowDenseSynthesizer.cpp'
$text = Read-Text $path
$old = @'
    hr = device->CreateUnorderedAccessView(m_regionRejectTexture, nullptr, &m_regionRejectUav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView(dense NVOF regional reject flag) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

    D3D11_TEXTURE2D_DESC denseDesc = {};
'@
$new = @'
    hr = device->CreateUnorderedAccessView(m_regionRejectTexture, nullptr, &m_regionRejectUav);
    if (FAILED(hr)) {
        status = std::format(L"CreateUnorderedAccessView(dense NVOF regional diagnostic) failed ({})", HR2Str(hr));
        Reset();
        return false;
    }

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
$text = Replace-Required $text $old $new 'staging telemetry creation'

$old = @'
    m_regionRejectUav.Release();
    m_regionRejectView.Release();
    m_regionRejectTexture.Release();
    m_unsafeCellUav.Release();
'@
$new = @'
    m_regionRejectUav.Release();
    m_regionRejectView.Release();
    m_regionRejectTexture.Release();
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
$text = Replace-Required $text $old $new 'telemetry reset'

$old = @'
    // A local catastrophic cluster can be visually unacceptable even when it
    // occupies far less than the global 25% threshold. Reject the entire
    // inserted midpoint rather than compositing real-frame patches locally.
    const RegionGateParameters regionValues = {
        // 18/49 (~36.7%) requires a genuinely dense catastrophic cluster.
        // The previous 8/49 threshold over-triggered on ordinary 23.976p
        // motion blur and effectively collapsed long stretches back to 24p.
        m_flowWidth, m_flowHeight, 18u, 3u,
    };
'@
$new = @'
    // Observe regional occupancy only. 8/49 and 18/49 both over-triggered on
    // ordinary 23.976p motion blur when used as whole-frame rejection rules.
    // The shader now records only the worst 7x7 cluster count for telemetry.
    const RegionGateParameters regionValues = {
        m_flowWidth, m_flowHeight, 0u, 3u,
    };
'@
$text = Replace-Required $text $old $new 'observe-only region parameters'

$old = @'
    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);
    UnbindCompute(context);

    UINT jumpStep = 1;
'@
$new = @'
    context->Dispatch((m_flowWidth + 7) / 8, (m_flowHeight + 7) / 8, 1);
    UnbindCompute(context);

    // Read a staging slot written three midpoint submissions ago. DO_NOT_WAIT
    // guarantees diagnostics can never stall video presentation. Whether or
    // not that slot is ready, queue the current 1x1 counters into it afterward.
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
$text = Replace-Required $text $old $new 'asynchronous telemetry readback'

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
$text = Replace-Required $text $old $new 'warp input bindings'
Write-Text $path $text

# Insert telemetry formatter after Reset().
$text = Read-Text $path
$marker = "bool CNvidiaOpticalFlowDenseSynthesizer::Dispatch(ID3D11DeviceContext* context,"
if (-not $text.Contains($marker)) { throw 'Dispatch marker not found for telemetry formatter.' }
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
        badPercent, m_lastUnsafeCount, cellCount,
        m_lastMaxLocalUnsafe,
        m_lastMaxLocalUnsafe >= 8 ? L"yes" : L"no",
        m_lastMaxLocalUnsafe >= 18 ? L"yes" : L"no");
}

'@
$text = $text.Replace($marker, $getter + $marker)
Write-Text $path $text

# Append observe-only telemetry to the live NVOF status line and update runtime text.
$path = 'Source/NvidiaOpticalFlowNative.cpp'
$text = Read-Text $path
$text = Replace-Required $text 'validated jump-flood dense flow + edge-aware next-frame warp + frame quality gate; live cost disabled' 'validated jump-flood dense flow + edge-aware next-frame warp + global frame quality gate; regional diagnostics observe-only; live cost disabled' 'runtime description'
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
$text = Replace-Required $text $old $new 'live telemetry status'
Write-Text $path $text

# Regenerate the two shaders whose source changed. Keep production runtime free
# of D3DCompile; these headers are compiled once on the Actions build host.
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
